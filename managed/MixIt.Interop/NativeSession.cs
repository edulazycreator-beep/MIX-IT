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
    TimeSpan VideoLatencyMax);

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
    long AvOffsetMaxAbsNanoseconds);

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

    /// <summary>Removes an input (only while not running).</summary>
    public void RemoveInput(uint inputId) =>
        Check(NativeMethods.mixit_session_remove_input(_handle, inputId), "mixit_session_remove_input");

    /// <summary>Selects the program input (0 = black). Takes effect at the next tick (a cut).</summary>
    public void SetProgramInput(uint inputId) =>
        Check(NativeMethods.mixit_session_set_program_input(_handle, inputId), "mixit_session_set_program_input");

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
            s.ProgramPoolExhausted, FromNs(s.VideoLatencyLastNs), FromNs(s.VideoLatencyMaxNs));
    }

    /// <summary>Counters of one input.</summary>
    public InputStatistics GetInputStatistics(uint inputId)
    {
        MixitInputStats s = default;
        s.StructSize = (uint)sizeof(MixitInputStats);
        Check(NativeMethods.mixit_session_get_input_stats(_handle, inputId, &s), "mixit_session_get_input_stats");
        return new InputStatistics(s.InputId, s.FramesProduced, s.FramesDropped, s.FramesPoolExhausted,
            s.FramesPresented, s.FramesRepeated, s.FramesSkipped, s.ProducerLateSkips, s.AudioBlocksProduced,
            s.AudioBlocksDropped, s.AudioBlocksConsumed, s.AvSyncEvents, s.AvOffsetLastNs, s.AvOffsetMaxAbsNs);
    }

    /// <summary>
    /// Copies the latest program frame (BGRA8) into <paramref name="destination"/>
    /// with rows <paramref name="stride"/> bytes apart. Returns null if no
    /// program frame exists yet (or the session is stopped).
    /// </summary>
    /// <exception cref="MixItNativeException">Buffer too small (Result = BufferTooSmall) or invalid stride.</exception>
    public ProgramFrameInfo? TryCopyProgramFrame(Span<byte> destination, int stride)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(stride);
        MixitVideoFrameInfo info = default;
        info.StructSize = (uint)sizeof(MixitVideoFrameInfo);
        NativeResult r;
        fixed (byte* p = destination)
        {
            r = NativeMethods.mixit_session_copy_program_frame(_handle, destination.IsEmpty ? null : p,
                (uint)destination.Length, (uint)stride, &info);
        }
        if (r == NativeResult.NotFound) return null;
        Check(r, "mixit_session_copy_program_frame");
        return ToInfo(info);
    }

    /// <summary>Size of the current program frame without copying it; null if none yet.</summary>
    public (int Width, int Height)? GetProgramFrameSize()
    {
        MixitVideoFrameInfo info = default;
        info.StructSize = (uint)sizeof(MixitVideoFrameInfo);
        NativeResult r = NativeMethods.mixit_session_copy_program_frame(_handle, null, 0, 0, &info);
        if (r == NativeResult.NotFound) return null;
        if (r != NativeResult.BufferTooSmall) Check(r, "mixit_session_copy_program_frame");
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

/// <summary>A codec present in the FFmpeg build.</summary>
public sealed record CodecInfo(string Name, bool Decoder, bool Encoder, bool Hardware, string LongName);
