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
    /// <summary>Microphone / line input, audio only (EXPERIMENTAL).</summary>
    AudioCapture,
    /// <summary>Titles, lower thirds, clock, scoreboard (M6).</summary>
    Graphics,
    /// <summary>Instant replay of another input (M6, video only).</summary>
    Replay,
}

/// <summary>Replay input status for the operator.</summary>
/// <param name="MarkNs">Marked in-point (master time), null when none.</param>
public sealed record ReplayStatus(uint ReplayInputId, uint SourceInputId, ReplayBufferInfo Buffer, ReplayState State, long? MarkNs);

/// <summary>An input of the current production, as the operator sees it.</summary>
public sealed record ProductionInput(uint Id, string Name, ProductionInputKind Kind, string Detail);

/// <summary>A recording or stream destination (M5). Ids are stable across session rebuilds.</summary>
/// <param name="SourceInputId">null = program (or the multiview, see <see cref="IsMultiview"/>); otherwise the input recorded (ISO).</param>
public sealed record ProductionOutput(int Id, string Name, OutputSettings Settings, uint? SourceInputId)
{
    public bool IsMultiview => Settings.SourceInputId == OutputSettings.MultiviewSource;
}

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
    private readonly List<OutputSpec> _outputs = [];
    private readonly OverlaySpec[] _overlays = Enumerable.Range(0, NativeSession.OverlayChannels).Select(_ => new OverlaySpec()).ToArray();
    private int _nextOutputId = 1;
    private NativeSession? _session;
    private ProductionFormat _format = ProductionFormat.Standard[0];
    private CompositorPreference _compositor = CompositorPreference.Auto;
    private uint? _pipInputId;
    private readonly Dictionary<AudioBus, AudioBusSettings> _buses = [];
    private AudioBus _monitorSource = AudioBus.Master;
    private (AudioBus Bus, string? DeviceId)? _audioOutput;
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
        DisposeSession();
        if (EngineAvailable) RequireSession();  // rebuild now: inputs, strips and the monitor output come back
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

    /// <summary>Audio devices of one direction (empty where unsupported).</summary>
    public IReadOnlyList<AudioDevice> GetAudioDevices(AudioDeviceDirection direction) =>
        EngineAvailable ? _host.EnumerateAudioDevices(direction) : [];

    /// <summary>Adds a microphone / line input (EXPERIMENTAL). Audio follows video is off: it is live.</summary>
    public ProductionInput AddAudioCapture(AudioDevice device)
    {
        ArgumentNullException.ThrowIfNull(device);
        RequireStopped("add inputs");
        ProductionInput input = AddSpec(new InputSpec(ProductionInputKind.AudioCapture, device.Name, device.Id, false));
        SetAudioStrip(input.Id, GetAudioStrip(input.Id) with { AudioFollowsVideo = false });
        return input;
    }

    public void RemoveInput(uint inputId)
    {
        RequireStopped("remove inputs");
        int index = _inputs.FindIndex(i => i.Id == inputId);
        if (index < 0) return;
        // Replays of this input go with it.
        foreach (InputSpec replay in _specs.Where(r => ReferenceEquals(r.ReplayOf, _specs[index])).ToList())
        {
            RemoveInput(_inputs[_specs.IndexOf(replay)].Id);
        }
        index = _inputs.FindIndex(i => i.Id == inputId);
        InputSpec? replayOf = _specs[index].ReplayOf;
        if (_pipInputId == inputId) SetPictureInPicture(null);
        NativeSession session = RequireSession();
        foreach (OutputSpec o in _outputs.Where(o => ReferenceEquals(o.Source, _specs[index])).ToList())
        {
            // An ISO recording of this input goes with it.
            if (o.NativeId is { } nid) session.RemoveOutput(nid);
            _outputs.Remove(o);
            _logger.LogInformation("Output {Name} removed with its input", o.Name);
        }
        for (int c = 0; c < _overlays.Length; c++)
        {
            if (!ReferenceEquals(_overlays[c].Source, _specs[index])) continue;
            _overlays[c] = new OverlaySpec();
            session.SetOverlay(c, new LayerSettings { InputId = 0 });
        }
        session.RemoveInput(inputId);
        _inputs.RemoveAt(index);
        _specs.RemoveAt(index);
        if (replayOf is not null && !_specs.Any(r => ReferenceEquals(r.ReplayOf, replayOf)))
        {
            // Last replay of that input: stop recording it (frees the buffer memory).
            replayOf.ReplaySeconds = 0;
            session.EnableReplay(_inputs[_specs.IndexOf(replayOf)].Id, 0);
        }
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

    // ---- VST3 inserts (EXPERIMENTAL) ----

    /// <summary>Scans for VST3 effects on a worker thread (loads every plug-in binary).</summary>
    public Task<IReadOnlyList<Vst3Plugin>> ScanPluginsAsync(string? path = null) =>
        EngineAvailable ? Task.Run(() => _host.ScanVst3(path)) : Task.FromResult<IReadOnlyList<Vst3Plugin>>([]);

    /// <summary>Insert slots of a strip (null = empty slot).</summary>
    public IReadOnlyList<AudioInsert?> GetInserts(uint inputId)
    {
        NativeSession session = RequireSession();
        var list = new AudioInsert?[NativeSession.AudioInsertSlots];
        for (int slot = 0; slot < list.Length; slot++) list[slot] = session.GetAudioInsert(inputId, slot);
        return list;
    }

    /// <summary>
    /// Loads a plug-in into a slot (on a worker thread: plug-ins can take seconds to load);
    /// kept, with its state, across rebuilds.
    /// </summary>
    public async Task SetInsertAsync(uint inputId, int slot, Vst3Plugin plugin)
    {
        ArgumentNullException.ThrowIfNull(plugin);
        _ = SpecOf(inputId);
        NativeSession session = RequireSession();
        await Task.Run(() => session.SetAudioInsert(inputId, slot, plugin.Path, plugin.ClassId));
        if (!ReferenceEquals(session, _session))
        {
            throw new InvalidOperationException("The production was rebuilt while the plug-in was loading; load it again.");
        }
        InputSpec spec = SpecOf(inputId);  // the input may have been removed meanwhile
        spec.Inserts[slot] = new InsertSpec(plugin) { State = session.GetAudioInsertState(inputId, slot) };
        _logger.LogInformation("Input {Id} insert {Slot}: {Plugin}", inputId, slot, plugin.DisplayName);
        RaiseChanged();
    }

    public void RemoveInsert(uint inputId, int slot)
    {
        InputSpec spec = SpecOf(inputId);
        RequireSession().RemoveAudioInsert(inputId, slot);
        spec.Inserts[slot] = null;
        RaiseChanged();
    }

    public void SetInsertBypass(uint inputId, int slot, bool bypass)
    {
        InputSpec spec = SpecOf(inputId);
        RequireSession().SetAudioInsertBypass(inputId, slot, bypass);
        if (spec.Inserts[slot] is { } insert) insert.Bypass = bypass;
    }

    public IReadOnlyList<AudioInsertParameter> GetInsertParameters(uint inputId, int slot) =>
        RequireSession().GetAudioInsertParameters(inputId, slot);

    public AudioInsertParameter GetInsertParameter(uint inputId, int slot, int index) =>
        RequireSession().GetAudioInsertParameter(inputId, slot, index);

    public void SetInsertParameter(uint inputId, int slot, uint parameterId, double normalized)
    {
        _ = SpecOf(inputId);
        RequireSession().SetAudioInsertParameter(inputId, slot, parameterId, normalized);
    }

    /// <summary>Selected monitor output device id (null = off; "" = default device).</summary>
    public string? AudioOutputDeviceId => _audioOutput?.DeviceId;

    /// <summary>Plays the monitor bus on a device (EXPERIMENTAL); null turns it off. Kept across rebuilds.</summary>
    public void SetAudioOutput(string? deviceId)
    {
        NativeSession session = RequireSession();
        if (deviceId is null)
        {
            session.DisableAudioOutput();
            _audioOutput = null;
            return;
        }
        session.SetAudioOutput(AudioBus.Monitor, deviceId);
        _audioOutput = (AudioBus.Monitor, deviceId);
    }

    public AudioOutputStatistics? GetAudioOutputStatistics()
    {
        if (_session is null) return null;
        try
        {
            return _session.GetAudioOutputStatistics();
        }
        catch (MixItNativeException ex) when (ex.Result == NativeResult.InvalidHandle)
        {
            return null;
        }
    }

    private void TryApplyOutput(NativeSession session, AudioBus bus, string? deviceId)
    {
        try
        {
            session.SetAudioOutput(bus, deviceId);
        }
        catch (MixItNativeException ex)
        {
            // The production must come back even if the headphone device is gone.
            _logger.LogWarning(ex, "Monitor output could not be restored on '{Device}'", deviceId);
            _audioOutput = null;
        }
    }

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

    private uint AddToSession(NativeSession session, InputSpec spec)
    {
        uint id = AddSourceToSession(session, spec);
        session.SetInputLabel(id, spec.Label ?? spec.Name);  // multiview label
        if (spec.ReplaySeconds > 0) session.EnableReplay(id, (uint)spec.ReplaySeconds, ReplayQuality);
        return id;
    }

    private uint AddSourceToSession(NativeSession session, InputSpec spec) => spec.Kind switch
    {
        ProductionInputKind.TestSignal => session.AddTestSource(new TestSourceOptions
        {
            Width = _format.Width, Height = _format.Height, FrameRate = _format.FrameRate,
            ToneHz = 1000, FlashIntervalFrames = 0,  // colour bars with a -20 dBFS line-up tone
        }),
        ProductionInputKind.CaptureDevice => session.AddCaptureSource(spec.Path!),
        ProductionInputKind.AudioCapture => session.AddAudioCaptureSource(spec.Path),
        ProductionInputKind.Graphics => AddGraphicsToSession(session, spec),
        ProductionInputKind.Replay => session.AddReplaySource(_inputs[_specs.IndexOf(spec.ReplayOf!)].Id),
        _ => session.AddFileSource(spec.Path!, spec.Loop),
    };

    private uint AddGraphicsToSession(NativeSession session, InputSpec spec)
    {
        uint id = session.AddGraphicsSource((uint)_format.Width, (uint)_format.Height, spec.Scene!);
        foreach ((string name, string value) in spec.Fields) session.SetGraphicsField(id, name, value);
        if (spec.Timer is { } t) session.SetGraphicsTimer(id, t.Mode, t.Value, false);
        return id;
    }

    private string Describe(InputSpec spec) => spec.Kind switch
    {
        ProductionInputKind.TestSignal => $"{_format.Width}x{_format.Height} @ {_format.FrameRate.Value:0.##}",
        ProductionInputKind.CaptureDevice => "Capture device (experimental)",
        ProductionInputKind.AudioCapture => "Audio input (experimental)",
        ProductionInputKind.Graphics => "Graphics",
        ProductionInputKind.Replay => $"Replay, last {spec.ReplayOf!.ReplaySeconds} s",
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
                if (_specs[i].Kind == ProductionInputKind.Replay) _specs[i].ReplayMark = null;  // new, empty buffer
                if (_specs[i].Strip is { } strip) session.SetAudioStrip(id, strip);
                ReapplyInserts(session, id, _specs[i]);
            }
            foreach ((AudioBus bus, AudioBusSettings settings) in _buses) session.SetAudioBus(bus, settings);
            foreach (OutputSpec spec in _outputs) spec.NativeId = session.AddOutput(NativeSettings(spec, session));
            for (int c = 0; c < _overlays.Length; c++) ApplyOverlay(session, c);
            session.SetMonitorSource(_monitorSource);
            if (_audioOutput is { } o) TryApplyOutput(session, o.Bus, o.DeviceId);
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

    // ---- graphics (M6) ----

    /// <summary>Adds a graphics input from a template (or custom scene JSON) with initial field values.</summary>
    public ProductionInput AddGraphics(string name, string sceneJson, IEnumerable<KeyValuePair<string, string>>? fields = null)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(name);
        ArgumentException.ThrowIfNullOrWhiteSpace(sceneJson);
        RequireStopped("add inputs");
        var spec = new InputSpec(ProductionInputKind.Graphics, name.Trim(), null, false) { Scene = sceneJson };
        foreach ((string k, string v) in fields ?? []) spec.Fields[k] = v;
        return AddSpec(spec);
    }

    public IReadOnlyDictionary<string, string> GetGraphicsFields(uint inputId) => GraphicsSpec(inputId).Fields;

    /// <summary>Changes a field on air (shown within ~10 ms).</summary>
    public void SetGraphicsField(uint inputId, string name, string value)
    {
        InputSpec spec = GraphicsSpec(inputId);
        RequireSession().SetGraphicsField(inputId, name, value);
        spec.Fields[name] = value;
    }

    public void SetGraphicsScene(uint inputId, string sceneJson)
    {
        InputSpec spec = GraphicsSpec(inputId);
        RequireSession().SetGraphicsScene(inputId, sceneJson);
        spec.Scene = sceneJson;
    }

    /// <summary>Stopwatch/countdown: <paramref name="value"/> null keeps the current value.</summary>
    public void SetGraphicsTimer(uint inputId, GraphicsTimerMode mode, TimeSpan? value, bool running)
    {
        InputSpec spec = GraphicsSpec(inputId);
        RequireSession().SetGraphicsTimer(inputId, mode, value, running);
        spec.Timer = (mode, value ?? spec.Timer?.Value);
    }

    public GraphicsInfo GetGraphicsInfo(uint inputId)
    {
        _ = GraphicsSpec(inputId);
        return RequireSession().GetGraphicsInfo(inputId);
    }

    private InputSpec GraphicsSpec(uint inputId)
    {
        int index = _inputs.FindIndex(i => i.Id == inputId);
        if (index < 0 || _specs[index].Kind != ProductionInputKind.Graphics) throw new ArgumentException($"Input {inputId} is not a graphics input.", nameof(inputId));
        return _specs[index];
    }

    // ---- overlay channels (M6) ----

    /// <summary>Puts an input on an overlay channel (null = empty), full frame unless <paramref name="destination"/> is given.</summary>
    public void SetOverlay(int channel, uint? inputId, KeySettings? key = null, NormalizedRect? destination = null, float opacity = 1.0f)
    {
        if (channel < 0 || channel >= _overlays.Length) throw new ArgumentOutOfRangeException(nameof(channel));
        InputSpec? source = null;
        if (inputId is { } id)
        {
            int index = _inputs.FindIndex(i => i.Id == id);
            if (index < 0) throw new ArgumentException($"No input {id}.", nameof(inputId));
            source = _specs[index];
        }
        var o = new OverlaySpec
        {
            Source = source,
            Layer = new LayerSettings { InputId = 0, Key = key ?? KeySettings.None, Destination = destination ?? NormalizedRect.Full, Opacity = opacity },
            Visible = source is not null && _overlays[channel].Visible,
        };
        _overlays[channel] = o;
        ApplyOverlay(RequireSession(), channel);
        RaiseChanged();
    }

    /// <summary>Takes an overlay in or out with a fade over <paramref name="fadeFrames"/> frames.</summary>
    public void ShowOverlay(int channel, bool visible, int fadeFrames = 0)
    {
        if (channel < 0 || channel >= _overlays.Length) throw new ArgumentOutOfRangeException(nameof(channel));
        RequireSession().ShowOverlay(channel, visible, fadeFrames);
        _overlays[channel].Visible = visible;
    }

    /// <summary>Input on the channel (null = empty), its key, and the engine's current state.</summary>
    public (uint? InputId, KeySettings Key, OverlayState State) GetOverlay(int channel)
    {
        OverlaySpec o = _overlays[channel];
        uint? id = o.Source is null ? null : _inputs[_specs.IndexOf(o.Source)].Id;
        return (id, o.Layer.Key, RequireSession().GetOverlay(channel));
    }

    /// <summary>Where the channel's input is drawn (normalized program coordinates).</summary>
    public NormalizedRect GetOverlayDestination(int channel) => _overlays[channel].Layer.Destination;

    private void ApplyOverlay(NativeSession session, int channel)
    {
        OverlaySpec o = _overlays[channel];
        int index = o.Source is null ? -1 : _specs.IndexOf(o.Source);
        uint id = index < 0 ? 0 : _inputs[index].Id;
        session.SetOverlay(channel, o.Layer with { InputId = id });
        if (id != 0 && o.Visible) session.ShowOverlay(channel, true, 0);
    }

    // ---- instant replay (M6) ----

    /// <summary>JPEG qscale of replay buffers (2 = best .. 31): visually clean at ~2-4 MB/s for 1080p.</summary>
    public const uint ReplayQuality = 4;

    /// <summary>
    /// Records <paramref name="sourceInputId"/>'s last <paramref name="seconds"/> (5..600) and adds a
    /// replay input playing it (not while running). Memory: roughly 3 MB per second at 1080p.
    /// </summary>
    public ProductionInput AddReplay(uint sourceInputId, int seconds = 60)
    {
        RequireStopped("add inputs");
        if (seconds is < 5 or > 600) throw new ArgumentOutOfRangeException(nameof(seconds), "5..600 seconds");
        InputSpec source = SpecOf(sourceInputId);
        if (source.Kind is ProductionInputKind.Replay or ProductionInputKind.AudioCapture)
            throw new InvalidOperationException("Replay needs an input with video (not a replay or audio input).");
        NativeSession session = RequireSession();
        int previous = source.ReplaySeconds;
        if (seconds > previous)
        {
            session.EnableReplay(sourceInputId, (uint)seconds, ReplayQuality);
            source.ReplaySeconds = seconds;
        }
        try
        {
            return AddSpec(new InputSpec(ProductionInputKind.Replay, $"Replay {source.Name}", null, false) { ReplayOf = source });
        }
        catch
        {
            if (source.ReplaySeconds != previous)
            {
                source.ReplaySeconds = previous;
                session.EnableReplay(sourceInputId, (uint)previous, ReplayQuality);
            }
            throw;
        }
    }

    public ReplayStatus GetReplayStatus(uint replayInputId)
    {
        InputSpec spec = ReplaySpec(replayInputId);
        NativeSession session = RequireSession();
        uint source = _inputs[_specs.IndexOf(spec.ReplayOf!)].Id;
        return new ReplayStatus(replayInputId, source, session.GetReplayBuffer(source), session.GetReplayState(replayInputId), spec.ReplayMark);
    }

    /// <summary>Marks "now" (the newest recorded frame) as the in-point.</summary>
    public long MarkReplayIn(uint replayInputId)
    {
        ReplayStatus status = GetReplayStatus(replayInputId);
        if (status.Buffer.Frames == 0) throw new InvalidOperationException("Nothing recorded yet (is production running?).");
        long mark = status.Buffer.NewestNs;
        ReplaySpec(replayInputId).ReplayMark = mark;
        return mark;
    }

    /// <summary>Plays from the mark, or from <paramref name="secondsBack"/> before the newest frame when there is no mark.</summary>
    public void PlayReplay(uint replayInputId, float speed, double secondsBack = 5, bool fromMark = true)
    {
        ReplayStatus status = GetReplayStatus(replayInputId);
        if (status.Buffer.Frames == 0) throw new InvalidOperationException("Nothing recorded yet (is production running?).");
        long from = fromMark && status.MarkNs is { } m ? m : status.Buffer.NewestNs - (long)(secondsBack * 1e9);
        RequireSession().ReplayPlay(replayInputId, Math.Max(from, status.Buffer.OldestNs), speed, false);
    }

    /// <summary>Speed and/or pause; null keeps the value.</summary>
    public void ControlReplay(uint replayInputId, float? speed, bool? paused)
    {
        ReplaySpec(replayInputId);
        RequireSession().ReplayControl(replayInputId, speed, paused);
    }

    private InputSpec ReplaySpec(uint inputId)
    {
        InputSpec spec = SpecOf(inputId);
        return spec.Kind == ProductionInputKind.Replay ? spec : throw new ArgumentException($"Input {inputId} is not a replay input.", nameof(inputId));
    }

    /// <summary>Renames an input (also its multiview label).</summary>
    public void RenameInput(uint inputId, string name)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(name);
        int index = _inputs.FindIndex(i => i.Id == inputId);
        if (index < 0) throw new ArgumentException($"No input {inputId}.", nameof(inputId));
        RequireSession().SetInputLabel(inputId, name.Trim());
        _specs[index].Label = name.Trim();  // the spec object is referenced by outputs/overlays/replays: mutate, never replace
        _inputs[index] = _inputs[index] with { Name = name.Trim() };
        RaiseChanged();
    }

    // ---- outputs: recording and streaming (M5) ----

    public IReadOnlyList<ProductionOutput> Outputs =>
        _outputs.Select(o => new ProductionOutput(o.Id, o.Name, o.Settings, SourceId(o))).ToList();

    /// <summary>Adds a destination (validated by the engine). <paramref name="isoInputId"/>: record that input instead of the program.</summary>
    public ProductionOutput AddOutput(string name, OutputSettings settings, uint? isoInputId = null)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(name);
        ArgumentNullException.ThrowIfNull(settings);
        InputSpec? source = null;
        if (isoInputId is { } iid)
        {
            int index = _inputs.FindIndex(i => i.Id == iid);
            if (index < 0) throw new ArgumentException($"No input {iid}.", nameof(isoInputId));
            source = _specs[index];
        }
        bool multiview = isoInputId is null && settings.SourceInputId == OutputSettings.MultiviewSource;
        var spec = new OutputSpec(_nextOutputId++, name.Trim(), settings with { SourceInputId = multiview ? OutputSettings.MultiviewSource : 0 }, source);
        NativeSession session = RequireSession();
        spec.NativeId = session.AddOutput(NativeSettings(spec, session));
        _outputs.Add(spec);
        _logger.LogInformation("Output {Name} added ({Kind})", spec.Name, settings.IsNetwork ? "stream" : "recording");
        RaiseChanged();
        return Outputs.Single(o => o.Id == spec.Id);
    }

    public void RemoveOutput(int outputId)
    {
        OutputSpec spec = OutputOf(outputId);
        if (spec.NativeId is { } nid && _session is not null)
        {
            OutputState state = _session.GetOutputStatistics(nid).State;
            if (state is not (OutputState.Idle or OutputState.Failed)) throw new InvalidOperationException("Stop the output first.");
            _session.RemoveOutput(nid);
        }
        _outputs.Remove(spec);
        RaiseChanged();
    }

    /// <summary>Starts recording/streaming (production must be running).</summary>
    public void StartOutput(int outputId)
    {
        if (!IsRunning) throw new InvalidOperationException("Start production before recording or streaming.");
        OutputSpec spec = OutputOf(outputId);
        RequireSession().StartOutput(spec.NativeId ?? throw new InvalidOperationException("Output not in the session."));
        _logger.LogInformation("Output {Name} started", spec.Name);
    }

    /// <summary>Stops gracefully (files are finalized in the background).</summary>
    public void StopOutput(int outputId)
    {
        OutputSpec spec = OutputOf(outputId);
        if (spec.NativeId is { } nid && _session is not null) _session.StopOutput(nid);
        _logger.LogInformation("Output {Name} stopping", spec.Name);
    }

    public OutputStatistics? GetOutputStatistics(int outputId)
    {
        OutputSpec spec = OutputOf(outputId);
        return spec.NativeId is { } nid && _session is not null ? _session.GetOutputStatistics(nid) : null;
    }

    /// <summary>H.264 encoders, open-tested on a worker thread.</summary>
    public Task<IReadOnlyList<VideoEncoderInfo>> QueryEncodersAsync() =>
        EngineAvailable ? Task.Run(NativeEngine.QueryVideoEncoders) : Task.FromResult<IReadOnlyList<VideoEncoderInfo>>([]);

    private OutputSpec OutputOf(int id) =>
        _outputs.FirstOrDefault(o => o.Id == id) ?? throw new ArgumentException($"No output {id}.", nameof(id));

    private uint? SourceId(OutputSpec o)
    {
        if (o.Source is null) return null;
        int index = _specs.IndexOf(o.Source);
        return index < 0 ? null : _inputs[index].Id;
    }

    private OutputSettings NativeSettings(OutputSpec o, NativeSession session)
    {
        _ = session;
        return o.Source is null ? o.Settings : o.Settings with { SourceInputId = SourceId(o) ?? 0 };
    }

    private sealed class OutputSpec(int id, string name, OutputSettings settings, InputSpec? source)
    {
        public int Id { get; } = id;
        public string Name { get; } = name;
        public OutputSettings Settings { get; } = settings;
        public InputSpec? Source { get; } = source;
        public uint? NativeId { get; set; }
    }

    private void RequireStopped(string action)
    {
        if (!CanEditInputs) throw new InvalidOperationException($"Stop production to {action}.");
    }

    private void ReapplyInserts(NativeSession session, uint id, InputSpec spec)
    {
        for (int slot = 0; slot < spec.Inserts.Length; slot++)
        {
            if (spec.Inserts[slot] is not { } insert) continue;
            try
            {
                session.SetAudioInsert(id, slot, insert.Plugin.Path, insert.Plugin.ClassId, insert.State);
                if (insert.Bypass) session.SetAudioInsertBypass(id, slot, true);
            }
            catch (MixItNativeException ex)
            {
                // A plug-in that no longer loads must not block the rest of the production.
                _logger.LogError("Input {Id} insert {Slot} ({Plugin}) could not be restored: {Message}", id, slot,
                    insert.Plugin.DisplayName, ex.Message);
                spec.Inserts[slot] = null;
            }
        }
    }

    /// <summary>Keeps the plug-in settings so a rebuilt session gets them back.</summary>
    private void SnapshotInserts()
    {
        if (_session is null) return;
        for (int i = 0; i < _specs.Count; i++)
        {
            for (int slot = 0; slot < _specs[i].Inserts.Length; slot++)
            {
                if (_specs[i].Inserts[slot] is not { } insert) continue;
                try
                {
                    insert.State = _session.GetAudioInsertState(_inputs[i].Id, slot);
                }
                catch (MixItNativeException)
                {
                    // Session already closed (engine stopped): keep the last snapshot.
                }
            }
        }
    }

    private InputSpec SpecOf(uint inputId)
    {
        int index = _inputs.FindIndex(i => i.Id == inputId);
        if (index < 0) throw new ArgumentException($"No input {inputId}.", nameof(inputId));
        return _specs[index];
    }

    private void DisposeSession()
    {
        SnapshotInserts();
        foreach (OutputSpec o in _outputs) o.NativeId = null;
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

        /// <summary>Plug-in inserts (null = empty slot).</summary>
        public InsertSpec?[] Inserts { get; } = new InsertSpec?[NativeSession.AudioInsertSlots];

        /// <summary>Graphics: scene JSON.</summary>
        public string? Scene { get; set; }

        /// <summary>Graphics: field values.</summary>
        public Dictionary<string, string> Fields { get; } = [];

        /// <summary>Graphics: last timer setting (restored paused after a rebuild).</summary>
        public (GraphicsTimerMode Mode, TimeSpan? Value)? Timer { get; set; }

        /// <summary>Operator's name after a rename (null = <see cref="Name"/>).</summary>
        public string? Label { get; set; }

        /// <summary>Seconds of this input kept for replay (0 = not recorded).</summary>
        public int ReplaySeconds { get; set; }

        /// <summary>Replay inputs: the input they replay.</summary>
        public InputSpec? ReplayOf { get; init; }

        /// <summary>Replay inputs: marked in-point (master time); lost on rebuild (the buffer restarts).</summary>
        public long? ReplayMark { get; set; }
    }

    private sealed class OverlaySpec
    {
        public InputSpec? Source { get; set; }
        public LayerSettings Layer { get; set; } = new() { InputId = 0 };
        public bool Visible { get; set; }
    }

    private sealed record InsertSpec(Vst3Plugin Plugin)
    {
        public byte[]? State { get; set; }
        public bool Bypass { get; set; }
    }
}
