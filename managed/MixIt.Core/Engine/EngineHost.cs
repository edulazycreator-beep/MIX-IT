using System.Runtime.ExceptionServices;
using Microsoft.Extensions.Logging;
using MixIt.Interop;

namespace MixIt.Core.Engine;

/// <summary>State of the engine as seen by the application.</summary>
public enum EngineHostState
{
    Stopped,
    Running,
    Faulted,
}

/// <summary>Configuration for <see cref="EngineHost"/>.</summary>
public sealed record EngineHostOptions
{
    /// <summary>Minimum level recorded by the engine.</summary>
    public NativeLogLevel MinimumLogLevel { get; init; } = NativeLogLevel.Information;

    /// <summary>JSON-lines log file written by the engine (null = no file).</summary>
    public string? LogFilePath { get; init; }

    /// <summary>Records buffered for the UI/consumer before the oldest are dropped.</summary>
    public int LogStreamCapacity { get; init; } = 2000;
}

/// <summary>
/// Owns the lifetime of the native engine for the application. Thread-safe;
/// state changes are serialized, and <see cref="StateChanged"/> is raised
/// outside the internal lock so handlers may call back into the host.
/// </summary>
public sealed class EngineHost : IDisposable
{
    private readonly object _gate = new();
    private readonly EngineHostOptions _options;
    private readonly ILogger<EngineHost> _logger;
    private volatile NativeEngine? _engine;
    private EngineHostState _state = EngineHostState.Stopped;
    private SystemCapabilities? _capabilities;

    /// <summary>Creates a host. The engine is not started until <see cref="Start"/>.</summary>
    public EngineHost(EngineHostOptions options, ILogger<EngineHost> logger)
    {
        ArgumentNullException.ThrowIfNull(options);
        ArgumentNullException.ThrowIfNull(logger);
        _options = options;
        _logger = logger;
        LogStream = new EngineLogStream(options.LogStreamCapacity);
    }

    /// <summary>Raised after every state transition (on the thread that caused it).</summary>
    public event EventHandler<EngineHostState>? StateChanged;

    /// <summary>Engine log records for the UI / tools.</summary>
    public EngineLogStream LogStream { get; }

    /// <summary>Current state.</summary>
    public EngineHostState State
    {
        get { lock (_gate) return _state; }
    }

    /// <summary>The error that put the host into <see cref="EngineHostState.Faulted"/>, if any.</summary>
    public Exception? LastError { get; private set; }

    /// <summary>Most recent capability snapshot, or null if not yet detected.</summary>
    public SystemCapabilities? Capabilities
    {
        get { lock (_gate) return _capabilities; }
    }

    /// <summary>Native library build description (loads the library).</summary>
    public static string NativeBuildInfo => NativeEngine.BuildInfo;

    /// <summary>Creates and initializes the native engine. No-op if already running.</summary>
    /// <exception cref="MixItNativeException">The engine failed to start; the host is then Faulted.</exception>
    public void Start()
    {
        MixItNativeException? failure = null;
        lock (_gate)
        {
            if (_state == EngineHostState.Running) return;
            try
            {
                _engine = NativeEngine.Create(new NativeEngineOptions
                {
                    MinimumLogLevel = _options.MinimumLogLevel,
                    LogFilePath = _options.LogFilePath,
                    LogHandler = LogStream.Publish,
                });
                LastError = null;
                _state = EngineHostState.Running;
            }
            catch (MixItNativeException ex)
            {
                LastError = failure = ex;
                _state = EngineHostState.Faulted;
            }
        }

        if (failure is not null)
        {
            _logger.LogError(failure, "Native engine failed to start");
            RaiseStateChanged(EngineHostState.Faulted);
            ExceptionDispatchInfo.Throw(failure);
        }
        _logger.LogInformation("Native engine started ({BuildInfo})", NativeEngine.BuildInfo);
        RaiseStateChanged(EngineHostState.Running);
    }

    /// <summary>Lifecycle step 2. Requires a running engine; caches the result.</summary>
    public SystemCapabilities DetectCapabilities()
    {
        NativeEngine engine = RequireEngine();
        SystemCapabilities caps = engine.DetectCapabilities();
        lock (_gate) _capabilities = caps;
        return caps;
    }

    /// <summary>Lifecycle step 3: creates a production session (engine must be running).</summary>
    public NativeSession CreateSession(SessionConfiguration configuration) => RequireEngine().CreateSession(configuration);

    /// <summary>Active audio devices (EXPERIMENTAL). Empty where no audio backend exists.</summary>
    public IReadOnlyList<AudioDevice> EnumerateAudioDevices(AudioDeviceDirection direction)
    {
        try
        {
            return RequireEngine().EnumerateAudioDevices(direction);
        }
        catch (MixItNativeException ex) when (ex.Result == NativeResult.NotSupported)
        {
            return [];
        }
    }

    /// <summary>VST3 effects in a bundle/folder, or the default folders when null (EXPERIMENTAL). Slow: not on the UI thread.</summary>
    public IReadOnlyList<Vst3Plugin> ScanVst3(string? path = null)
    {
        try
        {
            return RequireEngine().ScanVst3(path);
        }
        catch (MixItNativeException ex) when (ex.Result == NativeResult.NotSupported)
        {
            return [];
        }
    }

    /// <summary>Connected video capture devices (EXPERIMENTAL). Empty where capture is unsupported.</summary>
    public IReadOnlyList<CaptureDevice> EnumerateCaptureDevices()
    {
        try
        {
            return RequireEngine().EnumerateCaptureDevices();
        }
        catch (MixItNativeException ex) when (ex.Result == NativeResult.NotSupported)
        {
            return [];
        }
    }

    /// <summary>Engine health counters, or null when no engine is running.</summary>
    public EngineDiagnostics? GetDiagnostics()
    {
        NativeEngine? engine = _engine;
        if (engine is null) return null;
        try
        {
            return engine.GetDiagnostics();
        }
        catch (ObjectDisposedException)
        {
            return null;  // stopped concurrently
        }
    }

    /// <summary>Shuts the engine down and releases it. Idempotent.</summary>
    public void Stop()
    {
        lock (_gate)
        {
            NativeEngine? engine = _engine;
            if (engine is null)
            {
                if (_state == EngineHostState.Stopped) return;
            }
            else
            {
                _logger.LogInformation("Stopping native engine");
                _engine = null;
                try
                {
                    engine.Shutdown();
                }
                finally
                {
                    engine.Dispose();
                }
            }
            _state = EngineHostState.Stopped;
        }
        RaiseStateChanged(EngineHostState.Stopped);
    }

    /// <summary>Forwards a managed log record into the engine's ordered log. Never blocks.</summary>
    internal bool TryForwardLog(NativeLogLevel level, string category, string message) =>
        _engine?.TryLog(level, category, message) ?? false;

    internal bool IsRunning => _engine is not null;

    /// <inheritdoc />
    public void Dispose() => Stop();

    private NativeEngine RequireEngine() =>
        _engine ?? throw new InvalidOperationException("The native engine is not running. Call Start() first.");

    private void RaiseStateChanged(EngineHostState state)
    {
        try
        {
            StateChanged?.Invoke(this, state);
        }
        catch (Exception ex)
        {
            _logger.LogError(ex, "StateChanged handler threw");
        }
    }
}
