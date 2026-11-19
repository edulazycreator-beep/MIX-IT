using Microsoft.Extensions.Logging;
using MixIt.Core.Engine;
using MixIt.Interop;

namespace MixIt.Core.Production;

/// <summary>Kind of a production input.</summary>
public enum ProductionInputKind
{
    TestSignal,
    MediaFile,
    /// <summary>Webcam / capture card (EXPERIMENTAL).</summary>
    CaptureDevice,
}

/// <summary>An input of the current production, as the operator sees it.</summary>
public sealed record ProductionInput(uint Id, string Name, ProductionInputKind Kind, string Detail);

/// <summary>A selectable production format.</summary>
public sealed record ProductionFormat(string Name, uint Width, uint Height, MediaRate FrameRate)
{
    public static IReadOnlyList<ProductionFormat> Standard { get; } =
    [
        new("1080p 30", 1920, 1080, MediaRate.Fps30),
        new("1080p 29.97", 1920, 1080, MediaRate.Fps29_97),
        new("1080p 25", 1920, 1080, MediaRate.Fps25),
        new("1080p 60", 1920, 1080, MediaRate.Fps60),
        new("1080p 59.94", 1920, 1080, MediaRate.Fps59_94),
        new("1080p 50", 1920, 1080, MediaRate.Fps50),
        new("720p 60", 1280, 720, MediaRate.Fps60),
        new("720p 50", 1280, 720, MediaRate.Fps50),
        new("720p 30", 1280, 720, MediaRate.Fps30),
    ];

    public override string ToString() => Name;
}

/// <summary>
/// The operator's production: one engine session with its inputs, the
/// preview/program buses, transitions and a picture-in-picture overlay.
/// UI-free and single-threaded by contract (call from one thread, normally the
/// UI thread); <see cref="StartAsync"/> / <see cref="StopAsync"/> run the
/// blocking engine calls off that thread.
/// </summary>
/// <remarks>
/// The engine accepts new inputs only while the session is not running, so
/// <see cref="CanEditInputs"/> is false during production. If the engine is
/// restarted, the session is rebuilt from the recorded input list on next use.
/// </remarks>
public sealed class ProductionService : IDisposable
{
    private static readonly NormalizedRect PipRect = new(0.66f, 0.06f, 0.30f, 0.30f);

    private readonly EngineHost _host;
    private readonly ILogger<ProductionService> _logger;
    private readonly List<InputSpec> _specs = [];
    private readonly List<ProductionInput> _inputs = [];
    private NativeSession? _session;
    private ProductionFormat _format = ProductionFormat.Standard[0];
    private CompositorPreference _compositor = CompositorPreference.Auto;
    private uint? _pipInputId;
    private readonly Dictionary<AudioBus, AudioBusSettings> _buses = [];
    private AudioBus _monitorSource = AudioBus.Master;
    private int _nextTestNumber = 1;
    private bool _starting;

    public ProductionService(EngineHost host, ILogger<ProductionService> logger)
    {
        _host = host;
        _logger = logger;
    }

    /// <summary>Raised after any change the UI shows (inputs, running state, format).</summary>
    public event EventHandler? Changed;

    public IReadOnlyList<ProductionInput> Inputs => _inputs;

    public ProductionFormat Format => _format;

    public CompositorPreference Compositor => _compositor;

    /// <summary>True while the engine session produces frames.</summary>
    public bool IsRunning { get; private set; }

    public bool CanEditInputs => !IsRunning && !_starting;

    public uint? PictureInPictureInputId => _pipInputId;

    public bool EngineAvailable => _host.State == EngineHostState.Running;

    /// <summary>Changes the production format (not while running). Inputs are kept.</summary>
    public void SetFormat(ProductionFormat format, CompositorPreference compositor = CompositorPreference.Auto)
    {
        ArgumentNullException.ThrowIfNull(format);
        RequireStopped("change the format");
        if (format == _format && compositor == _compositor) return;
        _format = format;
        _compositor = compositor;
        DisposeSession();  // rebuilt with the new format on next use
        RaiseChanged();
    }

    public ProductionInput AddTestSignal()
    {
        RequireStopped("add inputs");
        int number = _nextTestNumber++;
        var spec = new InputSpec(ProductionInputKind.TestSignal, $"Test signal {number}", null, false);
        return AddSpec(spec);
    }

    public ProductionInput AddMediaFile(string path, bool loop = true)
    {
        ArgumentException.ThrowIfNullOrEmpty(path);
        RequireStopped("add inputs");
        var spec = new InputSpec(ProductionInputKind.MediaFile, Path.GetFileName(path), path, loop);
        return AddSpec(spec);
    }

    /// <summary>Connected capture devices (empty where capture is unsupported).</summary>
    public IReadOnlyList<CaptureDevice> GetCaptureDevices() => EngineAvailable ? _host.EnumerateCaptureDevices() : [];

    /// <summary>Adds a webcam / capture card input (EXPERIMENTAL).</summary>
    public ProductionInput AddCaptureDevice(CaptureDevice device)
    {
        ArgumentNullException.ThrowIfNull(device);
        RequireStopped("add inputs");
        return AddSpec(new InputSpec(ProductionInputKind.CaptureDevice, device.Name, device.Id, false));
    }

    public void RemoveInput(uint inputId)
    {
        RequireStopped("remove inputs");
        int index = _inputs.FindIndex(i => i.Id == inputId);
        if (index < 0) return;
        if (_pipInputId == inputId) SetPictureInPicture(null);
        RequireSession().RemoveInput(inputId);
        _inputs.RemoveAt(index);
        _specs.RemoveAt(index);
        RaiseChanged();
    }

    /// <summary>Starts production. Program shows the first input, preview the second.</summary>
    public async Task StartAsync()
    {
        if (IsRunning || _starting) return;
        NativeSession session = RequireSession();
        _starting = true;
        RaiseChanged();
        try
        {
            SessionStatistics s = session.GetStatistics();
            if (s.ProgramInputId == 0 && _inputs.Count > 0) session.SetProgramInput(_inputs[0].Id);
            if (s.PreviewInputId == 0 && _inputs.Count > 1) session.SetPreviewInput(_inputs[1].Id);
            await Task.Run(session.Start);
            IsRunning = true;
            _logger.LogInformation("Production started: {Format}, {Count} input(s)", _format.Name, _inputs.Count);
        }
        finally
        {
            _starting = false;
            RaiseChanged();
        }
    }

    public async Task StopAsync()
    {
        if (!IsRunning || _session is null) return;
        NativeSession session = _session;
        await Task.Run(session.Stop);
        IsRunning = false;
        _logger.LogInformation("Production stopped");
        RaiseChanged();
    }

    public void SetPreview(uint inputId) => RequireSession().SetPreviewInput(inputId);

    public void SetProgram(uint inputId) => RequireSession().SetProgramInput(inputId);

    public void Cut() => RequireSession().Cut();

    /// <summary>Starts an automatic transition; false if one is already running.</summary>
    public bool Auto(TransitionKind kind, uint durationFrames)
    {
        try
        {
            RequireSession().StartTransition(kind, durationFrames);
            return true;
        }
        catch (MixItNativeException ex) when (ex.Result == NativeResult.InvalidState)
        {
            return false;
        }
    }

    /// <summary>Shows an input as a picture-in-picture box on program (null = off).</summary>
    public void SetPictureInPicture(uint? inputId)
    {
        NativeSession session = RequireSession();
        if (inputId is uint id)
        {
            session.SetLayer(VideoBus.Program, 0, new LayerSettings { InputId = id, Destination = PipRect });
        }
        else
        {
            session.ClearLayer(VideoBus.Program, 0);
        }
        _pipInputId = inputId;
        RaiseChanged();
    }

    public SessionStatistics? GetStatistics() => _session?.GetStatistics();

    // ---- audio ------------------------------------------------------------------

    public AudioStripSettings GetAudioStrip(uint inputId) => RequireSession().GetAudioStrip(inputId);

    /// <summary>Applies a strip now (also while running) and keeps it for session rebuilds.</summary>
    public void SetAudioStrip(uint inputId, AudioStripSettings strip)
    {
        ArgumentNullException.ThrowIfNull(strip);
        int index = _inputs.FindIndex(i => i.Id == inputId);
        if (index < 0) throw new ArgumentException($"No input {inputId}.", nameof(inputId));
        RequireSession().SetAudioStrip(inputId, strip);
        _specs[index].Strip = strip;
    }

    public AudioBusSettings GetAudioBus(AudioBus bus) => RequireSession().GetAudioBus(bus);

    public void SetAudioBus(AudioBus bus, AudioBusSettings settings)
    {
        ArgumentNullException.ThrowIfNull(settings);
        RequireSession().SetAudioBus(bus, settings);
        _buses[bus] = settings;
    }

    public AudioBus MonitorSource => _monitorSource;

    public void SetMonitorSource(AudioBus bus)
    {
        RequireSession().SetMonitorSource(bus);
        _monitorSource = bus;
    }

    /// <summary>Meters, or null when no session exists (engine stopped).</summary>
    public AudioMeterReading? GetInputMeter(uint inputId)
    {
        if (_session is null) return null;
        try
        {
            return _session.GetInputMeter(inputId);
        }
        catch (MixItNativeException ex) when (ex.Result is NativeResult.NotFound or NativeResult.InvalidHandle)
        {
            return null;
        }
    }

    public AudioMeterReading? GetBusMeter(AudioBus bus)
    {
        if (_session is null) return null;
        try
        {
            return _session.GetBusMeter(bus);
        }
        catch (MixItNativeException ex) when (ex.Result == NativeResult.InvalidHandle)
        {
            return null;
        }
    }

    public void ResetLoudness(AudioBus bus) => RequireSession().ResetLoudness(bus);

    /// <summary>
    /// Copies the newest frame of a bus (BGRA8) into caller memory, e.g. a
    /// WriteableBitmap back buffer. Null when no frame is available.
    /// </summary>
    public ProgramFrameInfo? TryCopyBusFrame(VideoBus bus, nint destination, int length, int stride)
    {
        if (!IsRunning || _session is null) return null;
        try
        {
            return _session.TryCopyBusFrame(bus, destination, length, stride);
        }
        catch (MixItNativeException ex) when (ex.Result is NativeResult.InvalidState or NativeResult.InvalidHandle)
        {
            return null;  // engine stopped underneath us; HandleEngineStateChanged follows
        }
    }

    /// <summary>
    /// Call (on the owning thread) when the engine host changes state: the
    /// engine closes its sessions when it stops; the next use rebuilds them.
    /// </summary>
    public void HandleEngineStateChanged(EngineHostState state)
    {
        if (state == EngineHostState.Running || _session is null) return;
        DisposeSession();
        RaiseChanged();
    }

    public void Dispose() => DisposeSession();

    private ProductionInput AddSpec(InputSpec spec)
    {
        uint id = AddToSession(RequireSession(), spec);
        var input = new ProductionInput(id, spec.Name, spec.Kind, Describe(spec));
        _specs.Add(spec);
        _inputs.Add(input);
        _logger.LogInformation("Input {Id} added: {Name}", id, spec.Name);
        RaiseChanged();
        return input;
    }

    private uint AddToSession(NativeSession session, InputSpec spec) => spec.Kind switch
    {
        ProductionInputKind.TestSignal => session.AddTestSource(new TestSourceOptions
        {
            Width = _format.Width, Height = _format.Height, FrameRate = _format.FrameRate,
            ToneHz = 1000, FlashIntervalFrames = 0,  // colour bars with a -20 dBFS line-up tone
        }),
        ProductionInputKind.CaptureDevice => session.AddCaptureSource(spec.Path!),
        _ => session.AddFileSource(spec.Path!, spec.Loop),
    };

    private string Describe(InputSpec spec) => spec.Kind switch
    {
        ProductionInputKind.TestSignal => $"{_format.Width}x{_format.Height} @ {_format.FrameRate.Value:0.##}",
        ProductionInputKind.CaptureDevice => "Capture device (experimental)",
        _ => spec.Loop ? "Media file, looping" : "Media file",
    };

    private NativeSession RequireSession()
    {
        if (_session is not null && _session.GetStatistics().State != SessionState.Closed) return _session;
        DisposeSession();
        if (!EngineAvailable) throw new InvalidOperationException("The engine is not running.");

        var session = _host.CreateSession(new SessionConfiguration
        {
            Width = _format.Width, Height = _format.Height, FrameRate = _format.FrameRate, Compositor = _compositor,
        });
        // Rebuild inputs recorded before an engine restart or format change.
        try
        {
            for (int i = 0; i < _specs.Count; i++)
            {
                uint id = AddToSession(session, _specs[i]);
                _inputs[i] = _inputs[i] with { Id = id, Detail = Describe(_specs[i]) };
                if (_specs[i].Strip is { } strip) session.SetAudioStrip(id, strip);
            }
            foreach ((AudioBus bus, AudioBusSettings settings) in _buses) session.SetAudioBus(bus, settings);
            session.SetMonitorSource(_monitorSource);
        }
        catch
        {
            session.Dispose();
            throw;
        }
        _pipInputId = null;
        _session = session;
        return session;
    }

    private void RequireStopped(string action)
    {
        if (!CanEditInputs) throw new InvalidOperationException($"Stop production to {action}.");
    }

    private void DisposeSession()
    {
        _session?.Dispose();
        _session = null;
        IsRunning = false;
    }

    private void RaiseChanged() => Changed?.Invoke(this, EventArgs.Empty);

    /// <param name="Path">File path, or the device id for capture devices.</param>
    private sealed record InputSpec(ProductionInputKind Kind, string Name, string? Path, bool Loop)
    {
        /// <summary>Operator's strip settings (null = engine defaults); reapplied after a rebuild.</summary>
        public AudioStripSettings? Strip { get; set; }
    }
}
