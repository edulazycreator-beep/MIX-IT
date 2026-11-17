namespace MixIt.Interop;

/// <summary>An exact frame or sample rate, e.g. 30000/1001.</summary>
public readonly record struct MediaRate(uint Numerator, uint Denominator)
{
    public static MediaRate Fps23_976 => new(24000, 1001);
    public static MediaRate Fps24 => new(24, 1);
    public static MediaRate Fps25 => new(25, 1);
    public static MediaRate Fps29_97 => new(30000, 1001);
    public static MediaRate Fps30 => new(30, 1);
    public static MediaRate Fps50 => new(50, 1);
    public static MediaRate Fps59_94 => new(60000, 1001);
    public static MediaRate Fps60 => new(60, 1);

    /// <summary>Approximate value for display only (never for timing).</summary>
    public double Value => (double)Numerator / Denominator;

    public override string ToString() => Denominator == 1 ? $"{Numerator}" : $"{Numerator}/{Denominator}";
}

/// <summary>Session (production) format.</summary>
public sealed record SessionConfiguration
{
    public uint Width { get; init; } = 1920;
    public uint Height { get; init; } = 1080;
    public MediaRate FrameRate { get; init; } = MediaRate.Fps30;
    public uint AudioSampleRate { get; init; } = 48000;
    public uint AudioChannels { get; init; } = 2;
    /// <summary>0 = engine default.</summary>
    public uint ProgramPoolFrames { get; init; }
    /// <summary>
    /// Compositor to use. GPU preferences fall back to the CPU compositor (with
    /// a logged warning) when Direct3D 11 is unavailable; check
    /// <see cref="SessionStatistics.Compositor"/> for the one in use.
    /// </summary>
    public CompositorPreference Compositor { get; init; } = CompositorPreference.Auto;
}

/// <summary>Compositor preference (mirrors MIXIT_COMPOSITOR_PREFER_*).</summary>
public enum CompositorPreference
{
    /// <summary>The engine's choice (CPU in API 0.3).</summary>
    Auto = 0,
    Cpu = 1,
    Direct3D11 = 2,
    /// <summary>Direct3D 11 on the WARP software rasterizer (testing, no GPU).</summary>
    Direct3D11Software = 3,
}

/// <summary>Synthetic test-signal input options.</summary>
public sealed record TestSourceOptions
{
    public uint Width { get; init; } = 1920;
    public uint Height { get; init; } = 1080;
    public MediaRate FrameRate { get; init; } = MediaRate.Fps30;
    /// <summary>0 = no tone.</summary>
    public uint ToneHz { get; init; } = 1000;
    /// <summary>0 = no flash/click sync markers.</summary>
    public uint FlashIntervalFrames { get; init; }
}

/// <summary>Session lifecycle state (mirrors MIXIT_SESSION_STATE_*).</summary>
public enum SessionState
{
    Configuring = 1,
    Running = 2,
    Stopped = 3,
    Closed = 4,
}

/// <summary>Session counters.</summary>
public readonly record struct SessionStatistics(
    SessionState State,
    int InputCount,
    uint ProgramInputId,
    ulong Ticks,
    ulong LateTicks,
    ulong SkippedTicks,
    TimeSpan MaxTickLateness,
    ulong ProgramFrames,
    ulong ProgramBlackFrames,
    ulong ProgramFramesDropped,
    ulong ProgramPoolExhausted,
    TimeSpan VideoLatencyLast,
    TimeSpan VideoLatencyMax,
    uint PreviewInputId,
    TransitionKind? ActiveTransition,
    float TransitionProgress,
    CompositorKind Compositor,
    ulong TransitionsCompleted,
    ulong CompositedFrames,
    ulong CompositorFallbacks);

/// <summary>Video bus (mirrors MIXIT_BUS_*).</summary>
public enum VideoBus
{
    Program = 0,
    Preview = 1,
}

/// <summary>Transition between preview and program (mirrors MIXIT_TRANSITION_*).</summary>
public enum TransitionKind
{
    Cut = 0,
    /// <summary>Cross dissolve.</summary>
    Mix = 1,
    /// <summary>Fade through black.</summary>
    Fade = 2,
    /// <summary>Left-to-right wipe.</summary>
    Wipe = 3,
}

/// <summary>Compositor implementation in use (mirrors MIXIT_COMPOSITOR_*).</summary>
public enum CompositorKind
{
    Cpu = 0,
    Direct3D11 = 1,
}

/// <summary>A rectangle in normalized (0..1) frame coordinates.</summary>
public readonly record struct NormalizedRect(float X, float Y, float Width, float Height)
{
    public static NormalizedRect Full => new(0, 0, 1, 1);
}

/// <summary>
/// An overlay layer drawn on top of a bus's base input (picture-in-picture,
/// crops, partially transparent overlays).
/// </summary>
public sealed record LayerSettings
{
    public required uint InputId { get; init; }
    /// <summary>Region of the source to use.</summary>
    public NormalizedRect Crop { get; init; } = NormalizedRect.Full;
    /// <summary>Where the cropped source is drawn on the output (stretched to fit).</summary>
    public NormalizedRect Destination { get; init; } = NormalizedRect.Full;
    /// <summary>0..1, multiplied with the source alpha.</summary>
    public float Opacity { get; init; } = 1.0f;
}

/// <summary>Per-input counters, including A/V alignment of the test signal.</summary>
public readonly record struct InputStatistics(
    uint InputId,
    ulong FramesProduced,
    ulong FramesDropped,
    ulong FramesPoolExhausted,
    ulong FramesPresented,
    ulong FramesRepeated,
    ulong FramesSkipped,
    ulong ProducerLateSkips,
    ulong AudioBlocksProduced,
    ulong AudioBlocksDropped,
    ulong AudioBlocksConsumed,
    ulong AvSyncEvents,
    long AvOffsetLastNanoseconds,
    long AvOffsetMaxAbsNanoseconds,
    ulong DecodeErrors,
    ulong Loops);

/// <summary>Description of a copied program frame (BGRA8).</summary>
public readonly record struct ProgramFrameInfo(
    int Width,
    int Height,
    uint InputId,
    bool IsSyncFlash,
    long TickPtsNanoseconds,
    long SourcePtsNanoseconds,
    long SourceSequence,
    ulong TickIndex);

/// <summary>
/// A production session owned by a <see cref="NativeEngine"/>. Thread-safe.
/// Engine shutdown closes the session (state <see cref="SessionState.Closed"/>);
/// Dispose releases the handle.
/// </summary>
public sealed unsafe class NativeSession : IDisposable
{
    private readonly SessionHandle _handle;

    internal NativeSession(SessionHandle handle) => _handle = handle;

    /// <summary>Adds a synthetic test-signal input (only while not running). Returns its id.</summary>
    public uint AddTestSource(TestSourceOptions options)
    {
        ArgumentNullException.ThrowIfNull(options);
        var c = new MixitTestSourceConfig
        {
            StructSize = (uint)sizeof(MixitTestSourceConfig),
            Width = options.Width,
            Height = options.Height,
            FrameRateNum = options.FrameRate.Numerator,
            FrameRateDen = options.FrameRate.Denominator,
            ToneHz = options.ToneHz,
            FlashIntervalFrames = options.FlashIntervalFrames,
        };
        uint id;
        Check(NativeMethods.mixit_session_add_test_source(_handle, &c, &id), "mixit_session_add_test_source");
        return id;
    }

    /// <summary>
    /// Adds a local media file decoded with FFmpeg (only while not running).
    /// Throws with <see cref="NativeResult.NotFound"/> for a missing file and
    /// <see cref="NativeResult.NotSupported"/> when FFmpeg is unavailable.
    /// </summary>
    public uint AddFileSource(string path, bool loop = false)
    {
        ArgumentException.ThrowIfNullOrEmpty(path);
        byte* utf8 = (byte*)System.Runtime.InteropServices.Marshal.StringToCoTaskMemUTF8(path);
        try
        {
            var c = new MixitFileSourceConfig
            {
                StructSize = (uint)sizeof(MixitFileSourceConfig),
                Flags = loop ? 1u : 0u,
                Path = utf8,
            };
            uint id;
            Check(NativeMethods.mixit_session_add_file_source(_handle, &c, &id), "mixit_session_add_file_source");
            return id;
        }
        finally
        {
            System.Runtime.InteropServices.Marshal.FreeCoTaskMem((nint)utf8);
        }
    }

    /// <summary>
    /// Adds a video capture device input (EXPERIMENTAL; only while not running).
    /// Width/height 0 = largest mode up to 1080p; rate null = highest up to 60 fps.
    /// </summary>
    public uint AddCaptureSource(string deviceId, uint width = 0, uint height = 0, MediaRate? frameRate = null)
    {
        ArgumentException.ThrowIfNullOrEmpty(deviceId);
        byte* utf8 = (byte*)System.Runtime.InteropServices.Marshal.StringToCoTaskMemUTF8(deviceId);
        try
        {
            var c = new MixitCaptureSourceConfig
            {
                StructSize = (uint)sizeof(MixitCaptureSourceConfig),
                DeviceId = utf8,
                Width = width,
                Height = height,
                FrameRateNum = frameRate?.Numerator ?? 0,
                FrameRateDen = frameRate?.Denominator ?? 1,
            };
            uint id;
            Check(NativeMethods.mixit_session_add_capture_source(_handle, &c, &id), "mixit_session_add_capture_source");
            return id;
        }
        finally
        {
            System.Runtime.InteropServices.Marshal.FreeCoTaskMem((nint)utf8);
        }
    }

    /// <summary>Removes an input (only while not running).</summary>
    public void RemoveInput(uint inputId) =>
        Check(NativeMethods.mixit_session_remove_input(_handle, inputId), "mixit_session_remove_input");

    /// <summary>Selects the program input (0 = black). Takes effect at the next tick (a cut).</summary>
    public void SetProgramInput(uint inputId) =>
        Check(NativeMethods.mixit_session_set_program_input(_handle, inputId), "mixit_session_set_program_input");

    /// <summary>Selects the preview input (0 = black).</summary>
    public void SetPreviewInput(uint inputId) =>
        Check(NativeMethods.mixit_session_set_preview_input(_handle, inputId), "mixit_session_set_preview_input");

    /// <summary>Swaps preview and program at the next tick; completes a running transition.</summary>
    public void Cut() => Check(NativeMethods.mixit_session_cut(_handle), "mixit_session_cut");

    /// <summary>
    /// Transitions program to preview over <paramref name="durationFrames"/>
    /// output frames (1..600), then swaps the buses. While stopped the swap
    /// happens immediately. Throws with <see cref="NativeResult.InvalidState"/>
    /// if a transition is already running.
    /// </summary>
    public void StartTransition(TransitionKind kind, uint durationFrames) =>
        Check(NativeMethods.mixit_session_start_transition(_handle, (int)kind, durationFrames),
            "mixit_session_start_transition");

    /// <summary>Sets overlay layer <paramref name="index"/> (0..7) of a bus.</summary>
    public void SetLayer(VideoBus bus, uint index, LayerSettings layer)
    {
        ArgumentNullException.ThrowIfNull(layer);
        var c = new MixitLayerConfig
        {
            StructSize = (uint)sizeof(MixitLayerConfig),
            InputId = layer.InputId,
            CropX = layer.Crop.X,
            CropY = layer.Crop.Y,
            CropW = layer.Crop.Width,
            CropH = layer.Crop.Height,
            DestX = layer.Destination.X,
            DestY = layer.Destination.Y,
            DestW = layer.Destination.Width,
            DestH = layer.Destination.Height,
            Opacity = layer.Opacity,
        };
        Check(NativeMethods.mixit_session_set_layer(_handle, (int)bus, index, &c), "mixit_session_set_layer");
    }

    /// <summary>Removes overlay layer <paramref name="index"/> of a bus.</summary>
    public void ClearLayer(VideoBus bus, uint index) => SetLayer(bus, index, new LayerSettings { InputId = 0 });

    /// <summary>Starts production (idempotent).</summary>
    public void Start() => Check(NativeMethods.mixit_session_start(_handle), "mixit_session_start");

    /// <summary>Stops production (idempotent).</summary>
    public void Stop() => Check(NativeMethods.mixit_session_stop(_handle), "mixit_session_stop");

    /// <summary>Session counters.</summary>
    public SessionStatistics GetStatistics()
    {
        MixitSessionStats s = default;
        s.StructSize = (uint)sizeof(MixitSessionStats);
        Check(NativeMethods.mixit_session_get_stats(_handle, &s), "mixit_session_get_stats");
        return new SessionStatistics((SessionState)s.State, (int)s.InputCount, s.ProgramInputId, s.Ticks, s.LateTicks,
            s.SkippedTicks, FromNs(s.MaxTickLatenessNs), s.ProgramFrames, s.ProgramBlackFrames, s.ProgramFramesDropped,
            s.ProgramPoolExhausted, FromNs(s.VideoLatencyLastNs), FromNs(s.VideoLatencyMaxNs), s.PreviewInputId,
            s.TransitionActive < 0 ? null : (TransitionKind)s.TransitionActive, s.TransitionProgress,
            (CompositorKind)s.Compositor, s.TransitionsCompleted, s.CompositedFrames, s.CompositorFallbacks);
    }

    /// <summary>Counters of one input.</summary>
    public InputStatistics GetInputStatistics(uint inputId)
    {
        MixitInputStats s = default;
        s.StructSize = (uint)sizeof(MixitInputStats);
        Check(NativeMethods.mixit_session_get_input_stats(_handle, inputId, &s), "mixit_session_get_input_stats");
        return new InputStatistics(s.InputId, s.FramesProduced, s.FramesDropped, s.FramesPoolExhausted,
            s.FramesPresented, s.FramesRepeated, s.FramesSkipped, s.ProducerLateSkips, s.AudioBlocksProduced,
            s.AudioBlocksDropped, s.AudioBlocksConsumed, s.AvSyncEvents, s.AvOffsetLastNs, s.AvOffsetMaxAbsNs,
            s.DecodeErrors, s.Loops);
    }

    /// <summary>
    /// Copies the latest program frame (BGRA8) into <paramref name="destination"/>
    /// with rows <paramref name="stride"/> bytes apart. Returns null if no
    /// program frame exists yet (or the session is stopped).
    /// </summary>
    /// <exception cref="MixItNativeException">Buffer too small (Result = BufferTooSmall) or invalid stride.</exception>
    public ProgramFrameInfo? TryCopyProgramFrame(Span<byte> destination, int stride) =>
        TryCopyBusFrame(VideoBus.Program, destination, stride);

    /// <summary>
    /// Copies the latest frame of a bus (BGRA8). Preview is rendered on demand:
    /// the first request after a pause returns null until the next tick.
    /// Returns null if no frame exists yet (or the session is stopped).
    /// </summary>
    public ProgramFrameInfo? TryCopyBusFrame(VideoBus bus, Span<byte> destination, int stride)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(stride);
        MixitVideoFrameInfo info = default;
        info.StructSize = (uint)sizeof(MixitVideoFrameInfo);
        NativeResult r;
        fixed (byte* p = destination)
        {
            r = NativeMethods.mixit_session_copy_bus_frame(_handle, (int)bus, destination.IsEmpty ? null : p,
                (uint)destination.Length, (uint)stride, &info);
        }
        if (r == NativeResult.NotFound) return null;
        Check(r, "mixit_session_copy_bus_frame");
        return ToInfo(info);
    }

    /// <summary>
    /// Copies the latest frame of a bus into unmanaged memory (e.g. a
    /// WriteableBitmap back buffer) of <paramref name="length"/> bytes.
    /// </summary>
    public ProgramFrameInfo? TryCopyBusFrame(VideoBus bus, nint destination, int length, int stride)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(length);
        return TryCopyBusFrame(bus, new Span<byte>((void*)destination, length), stride);
    }

    /// <summary>Size of the current program frame without copying it; null if none yet.</summary>
    public (int Width, int Height)? GetProgramFrameSize() => GetBusFrameSize(VideoBus.Program);

    /// <summary>Size of the current frame of a bus without copying it; null if none yet.</summary>
    public (int Width, int Height)? GetBusFrameSize(VideoBus bus)
    {
        MixitVideoFrameInfo info = default;
        info.StructSize = (uint)sizeof(MixitVideoFrameInfo);
        NativeResult r = NativeMethods.mixit_session_copy_bus_frame(_handle, (int)bus, null, 0, 0, &info);
        if (r == NativeResult.NotFound) return null;
        if (r != NativeResult.BufferTooSmall) Check(r, "mixit_session_copy_bus_frame");
        return ((int)info.Width, (int)info.Height);
    }

    /// <summary>Stops the session if running and releases it.</summary>
    public void Dispose() => _handle.Dispose();

    private static ProgramFrameInfo ToInfo(in MixitVideoFrameInfo i) =>
        new((int)i.Width, (int)i.Height, i.InputId, (i.Flags & 1u) != 0, i.TickPtsNs, i.SourcePtsNs, i.SourceSequence,
            i.TickIndex);

    private static TimeSpan FromNs(long ns) => TimeSpan.FromTicks(ns / 100);

    private static void Check(NativeResult result, string function)
    {
        if (result != NativeResult.Ok)
        {
            throw new MixItNativeException(result, function, NativeMethods.GetLastErrorMessage());
        }
    }
}

/// <summary>FFmpeg availability and licence verdict.</summary>
public sealed record MediaLibraryInfo(
    bool CompiledIn,
    bool VersionsMatch,
    bool LicenseIsLgpl,
    bool LgplRequired,
    bool Usable,
    string Version,
    string License,
    Version? AvcodecVersion);

/// <summary>A video capture device: <see cref="Id"/> is passed to AddCaptureSource.</summary>
public sealed record CaptureDevice(string Id, string Name);

/// <summary>A codec present in the FFmpeg build.</summary>
public sealed record CodecInfo(string Name, bool Decoder, bool Encoder, bool Hardware, string LongName);
