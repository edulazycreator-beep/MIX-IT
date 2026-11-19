namespace MixIt.Interop;

/// <summary>Audio buses (mirrors MIXIT_AUDIO_BUS_*).</summary>
public enum AudioBus
{
    Master = 0,
    AuxA = 1,
    AuxB = 2,
    AuxC = 3,
    AuxD = 4,
    /// <summary>Listen bus: soloed strips, else a copy of the monitor source. Read-only.</summary>
    Monitor = 5,
}

/// <summary>EQ band shape (mirrors MIXIT_EQ_*).</summary>
public enum EqType
{
    Peaking = 0,
    LowShelf = 1,
    HighShelf = 2,
    HighPass = 3,
    LowPass = 4,
}

public sealed record EqBandSettings(bool Enabled, EqType Type, float FrequencyHz, float GainDb, float Q);

public sealed record GateSettings
{
    public bool Enabled { get; init; }
    public float ThresholdDb { get; init; } = -50f;
    /// <summary>0 = gate (attenuate by Range), 1..20 = downward expander ratio.</summary>
    public float Ratio { get; init; }
    public float RangeDb { get; init; } = -60f;
    public float AttackMs { get; init; } = 1f;
    public float HoldMs { get; init; } = 50f;
    public float ReleaseMs { get; init; } = 200f;
}

public sealed record CompressorSettings
{
    public bool Enabled { get; init; }
    public float ThresholdDb { get; init; } = -18f;
    public float Ratio { get; init; } = 3f;
    public float KneeDb { get; init; } = 6f;
    public float AttackMs { get; init; } = 10f;
    public float ReleaseMs { get; init; } = 150f;
    public float MakeupDb { get; init; }
}

/// <summary>
/// One input's channel strip. Flow: trim → high-pass → gate → EQ → compressor
/// → fader × mute × audio-follows-video → balance → buses.
/// </summary>
public sealed record AudioStripSettings
{
    /// <summary>At or below this the fader is off.</summary>
    public const float FaderOffDb = -96f;

    public float TrimDb { get; init; }
    public float FaderDb { get; init; }
    public float Balance { get; init; }
    public bool Mute { get; init; }
    /// <summary>Affects the monitor bus only (after-fader listen).</summary>
    public bool Solo { get; init; }
    /// <summary>Audible only while the input is on program (default).</summary>
    public bool AudioFollowsVideo { get; init; } = true;
    /// <summary>Bit 0 master, bits 1..4 aux A..D.</summary>
    public uint BusMask { get; init; } = 1;
    public float DelayMs { get; init; }
    public bool HighPass { get; init; }
    public float HighPassHz { get; init; } = 80f;
    public IReadOnlyList<EqBandSettings> Eq { get; init; } =
    [
        new(false, EqType.LowShelf, 100f, 0f, 0.707f),
        new(false, EqType.Peaking, 500f, 0f, 1f),
        new(false, EqType.Peaking, 2500f, 0f, 1f),
        new(false, EqType.HighShelf, 8000f, 0f, 0.707f),
    ];
    public GateSettings Gate { get; init; } = new();
    public CompressorSettings Compressor { get; init; } = new();

    public bool SendsTo(AudioBus bus) => bus != AudioBus.Monitor && (BusMask & (1u << (int)bus)) != 0;
}

public sealed record AudioBusSettings
{
    public float FaderDb { get; init; }
    public bool Mute { get; init; }
    public bool Limiter { get; init; }
    public float LimiterCeilingDb { get; init; } = -1f;
    public float LimiterReleaseMs { get; init; } = 100f;
}

/// <summary>Meter values (dBFS; -144 = silence).</summary>
public sealed record AudioMeterReading(
    int Channels,
    IReadOnlyList<float> PeakDb,
    IReadOnlyList<float> RmsDb,
    float GainReductionDb,
    float GateReductionDb,
    bool GateOpen,
    float AppliedGainDb,
    float MomentaryLufs,
    float ShortTermLufs,
    float IntegratedLufs)
{
    public const float Silence = -144f;
}

/// <summary>Position of a block of bus audio on the session timeline.</summary>
public readonly record struct AudioBlockInfo(int Frames, TimeSpan MasterClockTime, long FirstSample);

internal static unsafe class AudioConvert
{
    public static MixitAudioStrip ToNative(AudioStripSettings s)
    {
        if (s.Eq.Count != 4) throw new ArgumentException("Exactly 4 EQ bands are required.", nameof(s));
        var c = new MixitAudioStrip
        {
            StructSize = (uint)sizeof(MixitAudioStrip),
            Flags = (s.Mute ? 0x1u : 0) | (s.Solo ? 0x2u : 0) | (s.AudioFollowsVideo ? 0x4u : 0) | (s.HighPass ? 0x8u : 0) |
                    (s.Gate.Enabled ? 0x10u : 0) | (s.Compressor.Enabled ? 0x20u : 0),
            TrimDb = s.TrimDb,
            FaderDb = s.FaderDb,
            Balance = s.Balance,
            DelayMs = s.DelayMs,
            BusMask = s.BusMask,
            HighPassHz = s.HighPassHz,
            Eq0 = Band(s.Eq[0]),
            Eq1 = Band(s.Eq[1]),
            Eq2 = Band(s.Eq[2]),
            Eq3 = Band(s.Eq[3]),
            GateThresholdDb = s.Gate.ThresholdDb,
            GateRatio = s.Gate.Ratio,
            GateRangeDb = s.Gate.RangeDb,
            GateAttackMs = s.Gate.AttackMs,
            GateHoldMs = s.Gate.HoldMs,
            GateReleaseMs = s.Gate.ReleaseMs,
            CompThresholdDb = s.Compressor.ThresholdDb,
            CompRatio = s.Compressor.Ratio,
            CompKneeDb = s.Compressor.KneeDb,
            CompAttackMs = s.Compressor.AttackMs,
            CompReleaseMs = s.Compressor.ReleaseMs,
            CompMakeupDb = s.Compressor.MakeupDb,
        };
        return c;

        static MixitEqBand Band(EqBandSettings b) => new()
        {
            Enabled = b.Enabled ? 1 : 0, Type = (int)b.Type, FrequencyHz = b.FrequencyHz, GainDb = b.GainDb, Q = b.Q,
        };
    }

    public static AudioStripSettings FromNative(in MixitAudioStrip c) => new()
    {
        Mute = (c.Flags & 0x1) != 0,
        Solo = (c.Flags & 0x2) != 0,
        AudioFollowsVideo = (c.Flags & 0x4) != 0,
        HighPass = (c.Flags & 0x8) != 0,
        TrimDb = c.TrimDb,
        FaderDb = c.FaderDb,
        Balance = c.Balance,
        DelayMs = c.DelayMs,
        BusMask = c.BusMask,
        HighPassHz = c.HighPassHz,
        Eq = [Band(c.Eq0), Band(c.Eq1), Band(c.Eq2), Band(c.Eq3)],
        Gate = new GateSettings
        {
            Enabled = (c.Flags & 0x10) != 0, ThresholdDb = c.GateThresholdDb, Ratio = c.GateRatio, RangeDb = c.GateRangeDb,
            AttackMs = c.GateAttackMs, HoldMs = c.GateHoldMs, ReleaseMs = c.GateReleaseMs,
        },
        Compressor = new CompressorSettings
        {
            Enabled = (c.Flags & 0x20) != 0, ThresholdDb = c.CompThresholdDb, Ratio = c.CompRatio, KneeDb = c.CompKneeDb,
            AttackMs = c.CompAttackMs, ReleaseMs = c.CompReleaseMs, MakeupDb = c.CompMakeupDb,
        },
    };

    private static EqBandSettings Band(MixitEqBand b) => new(b.Enabled != 0, (EqType)b.Type, b.FrequencyHz, b.GainDb, b.Q);

    public static AudioMeterReading FromNative(MixitAudioMeter m)
    {
        int n = (int)Math.Min(m.Channels, 8u);
        var peak = new float[n];
        var rms = new float[n];
        for (int i = 0; i < n; i++)
        {
            peak[i] = m.PeakDb[i];  // m is a local copy: its fixed buffers need no pinning
            rms[i] = m.RmsDb[i];
        }
        return new AudioMeterReading(n, peak, rms, m.GainReductionDb, m.GateReductionDb, (m.Flags & 1) != 0, m.AppliedGainDb,
            m.MomentaryLufs, m.ShortTermLufs, m.IntegratedLufs);
    }
}
