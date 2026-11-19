using MixIt.Interop;
using Xunit;

namespace MixIt.Tests;

/// <summary>Managed session API against the real engine (short real-time runs).</summary>
public class SessionTests
{
    private static readonly SessionConfiguration Small = new()
    {
        Width = 640, Height = 360, FrameRate = MediaRate.Fps30,
    };

    [Fact]
    public void RunsTestSignalToProgramAndReportsStatistics()
    {
        using var engine = NativeEngine.Create(new NativeEngineOptions());
        using NativeSession session = engine.CreateSession(Small);
        uint input = session.AddTestSource(new TestSourceOptions
        {
            Width = 640, Height = 360, FrameRate = MediaRate.Fps30, FlashIntervalFrames = 10,
        });
        Assert.Equal(1u, input);
        session.SetProgramInput(input);
        session.Start();
        Thread.Sleep(1200);

        Assert.Equal((640, 360), session.GetProgramFrameSize());
        var buffer = new byte[640 * 360 * 4];
        ProgramFrameInfo? info = session.TryCopyProgramFrame(buffer, 640 * 4);
        Assert.NotNull(info);
        Assert.Equal(input, info.Value.InputId);
        Assert.True(info.Value.SourcePtsNanoseconds <= info.Value.TickPtsNanoseconds);
        Assert.Contains(buffer, b => b != 0);

        session.Stop();
        SessionStatistics s = session.GetStatistics();
        InputStatistics i = session.GetInputStatistics(input);
        Assert.Equal(SessionState.Stopped, s.State);
        Assert.InRange(s.Ticks, 25UL, 45UL);
        Assert.Equal(0UL, i.FramesDropped);
        Assert.True(i.AvSyncEvents >= 2, $"A/V events {i.AvSyncEvents}");
        Assert.InRange(i.AvOffsetMaxAbsNanoseconds, 0L, 10_417L);
        Assert.Null(session.TryCopyProgramFrame(buffer, 640 * 4));  // stopped sessions hold no frames
    }

    [Fact]
    public void InvalidOperationsReportNativeErrors()
    {
        using var engine = NativeEngine.Create(new NativeEngineOptions());
        var ex = Assert.Throws<MixItNativeException>(() =>
            engine.CreateSession(Small with { FrameRate = new MediaRate(29, 1) }));
        Assert.Equal(NativeResult.NotSupported, ex.Result);

        using NativeSession session = engine.CreateSession(Small);
        Assert.Equal(NativeResult.NotFound,
            Assert.Throws<MixItNativeException>(() => session.SetProgramInput(42)).Result);
        uint input = session.AddTestSource(new TestSourceOptions { Width = 640, Height = 360 });
        session.SetProgramInput(input);
        session.Start();
        Assert.Equal(NativeResult.InvalidState,
            Assert.Throws<MixItNativeException>(() => session.AddTestSource(new TestSourceOptions())).Result);
        Thread.Sleep(200);
        var tooSmall = new byte[16];
        Assert.Equal(NativeResult.BufferTooSmall,
            Assert.Throws<MixItNativeException>(() => session.TryCopyProgramFrame(tooSmall, 640 * 4)).Result);
    }

    [Fact]
    public void EngineDisposeClosesSessions()
    {
        var engine = NativeEngine.Create(new NativeEngineOptions());
        using NativeSession session = engine.CreateSession(Small);
        session.AddTestSource(new TestSourceOptions { Width = 640, Height = 360 });
        session.Start();
        engine.Dispose();
        Assert.Equal(SessionState.Closed, session.GetStatistics().State);
        Assert.Equal(NativeResult.InvalidState, Assert.Throws<MixItNativeException>(() => session.Start()).Result);
    }

    [Fact]
    public void MediaLibraryInfoFollowsThePolicy()
    {
        MediaLibraryInfo m = NativeEngine.GetMediaLibraryInfo();
        Assert.Equal(m.CompiledIn && m.VersionsMatch && (m.LicenseIsLgpl || !m.LgplRequired), m.Usable);
        if (!m.Usable)
        {
            Assert.Null(NativeEngine.QueryCodec("h264"));
            return;
        }
        CodecInfo? h264 = NativeEngine.QueryCodec("h264");
        Assert.NotNull(h264);
        Assert.True(h264.Decoder);
        Assert.Null(NativeEngine.QueryCodec("definitely_not_a_codec"));
    }

    [Fact]
    public void FileSourceErrorsAreMapped()
    {
        using var engine = NativeEngine.Create(new NativeEngineOptions());
        using NativeSession session = engine.CreateSession(Small);
        string missing = Path.Combine(Path.GetTempPath(), "mixit-missing-" + Guid.NewGuid() + ".mp4");
        var ex = Assert.Throws<MixItNativeException>(() => session.AddFileSource(missing));
        NativeResult expected = NativeEngine.GetMediaLibraryInfo().Usable ? NativeResult.NotFound : NativeResult.NotSupported;
        Assert.Equal(expected, ex.Result);
        Assert.Equal("mixit_session_add_file_source", ex.Function);
    }

    [Fact]
    public void MediaRateFormatting()
    {
        Assert.Equal("30000/1001", MediaRate.Fps29_97.ToString());
        Assert.Equal("25", MediaRate.Fps25.ToString());
        Assert.InRange(MediaRate.Fps59_94.Value, 59.94, 59.941);
    }

    [Fact]
    public void PreviewCutTransitionAndLayers()
    {
        using var engine = NativeEngine.Create(new NativeEngineOptions());
        using NativeSession session = engine.CreateSession(Small);
        var source = new TestSourceOptions { Width = 640, Height = 360, FrameRate = MediaRate.Fps30 };
        uint a = session.AddTestSource(source);
        uint b = session.AddTestSource(source);
        session.SetProgramInput(a);
        session.SetPreviewInput(b);
        session.SetLayer(VideoBus.Program, 0, new LayerSettings
        {
            InputId = b, Destination = new NormalizedRect(0.5f, 0.5f, 0.5f, 0.5f), Opacity = 0.75f,
        });
        session.Start();

        var buffer = new byte[640 * 360 * 4];
        ProgramFrameInfo? preview = null;
        for (int i = 0; i < 25 && preview is null; i++)
        {
            Thread.Sleep(40);
            preview = session.TryCopyBusFrame(VideoBus.Preview, buffer, 640 * 4);
        }
        Assert.NotNull(preview);
        Assert.Equal(b, preview.Value.InputId);
        Assert.Equal(a, session.TryCopyProgramFrame(buffer, 640 * 4)?.InputId);

        session.Cut();
        Thread.Sleep(150);
        SessionStatistics s = session.GetStatistics();
        Assert.Equal(b, s.ProgramInputId);
        Assert.Equal(a, s.PreviewInputId);
        Assert.Equal(CompositorKind.Cpu, s.Compositor);
        Assert.True(s.CompositedFrames > 0, "the PiP layer is composited");

        session.StartTransition(TransitionKind.Mix, 15);
        Assert.Equal(NativeResult.InvalidState,
            Assert.Throws<MixItNativeException>(() => session.StartTransition(TransitionKind.Wipe, 15)).Result);
        Thread.Sleep(900);
        s = session.GetStatistics();
        Assert.Null(s.ActiveTransition);
        Assert.Equal(a, s.ProgramInputId);
        Assert.Equal(2UL, s.TransitionsCompleted);

        Assert.Equal(NativeResult.InvalidArgument,
            Assert.Throws<MixItNativeException>(() => session.SetLayer(VideoBus.Program, 8, new LayerSettings { InputId = a })).Result);
        session.ClearLayer(VideoBus.Program, 0);
    }

    [Fact]
    public void GpuCompositorPreferenceIsHonouredOrFallsBack()
    {
        using var engine = NativeEngine.Create(new NativeEngineOptions());
        using NativeSession session = engine.CreateSession(Small with { Compositor = CompositorPreference.Direct3D11Software });
        CompositorKind expected = OperatingSystem.IsWindows() ? CompositorKind.Direct3D11 : CompositorKind.Cpu;
        Assert.Equal(expected, session.GetStatistics().Compositor);
        Assert.Equal(NativeResult.InvalidArgument, Assert.Throws<MixItNativeException>(() =>
            engine.CreateSession(Small with { Compositor = (CompositorPreference)9 })).Result);
    }

    [Fact]
    public void AudioMixerStripsBusesMetersAndAudioRead()
    {
        using var engine = NativeEngine.Create(new NativeEngineOptions());
        using NativeSession session = engine.CreateSession(Small);
        var source = new TestSourceOptions { Width = 640, Height = 360, FrameRate = MediaRate.Fps30, ToneHz = 1000 };
        uint a = session.AddTestSource(source);
        AudioStripSettings strip = session.GetAudioStrip(a);
        Assert.True(strip.AudioFollowsVideo);
        Assert.True(strip.SendsTo(AudioBus.Master));
        session.SetAudioStrip(a, strip with { FaderDb = -6f, Compressor = strip.Compressor with { Enabled = true } });
        Assert.Equal(-6f, session.GetAudioStrip(a).FaderDb);
        Assert.True(session.GetAudioStrip(a).Compressor.Enabled);
        Assert.Equal(NativeResult.InvalidArgument,
            Assert.Throws<MixItNativeException>(() => session.SetAudioStrip(a, strip with { FaderDb = 40f })).Result);

        AudioBusSettings master = session.GetAudioBus(AudioBus.Master);
        Assert.True(master.Limiter);
        session.SetAudioBus(AudioBus.AuxB, new AudioBusSettings { FaderDb = -3f });
        Assert.Equal(-3f, session.GetAudioBus(AudioBus.AuxB).FaderDb);
        session.SetMonitorSource(AudioBus.AuxA);

        session.SetProgramInput(a);
        session.Start();
        Thread.Sleep(500);
        var buffer = new float[480 * 2 * 20];
        AudioBlockInfo? block = session.TryReadBusAudio(AudioBus.Master, buffer, 2);
        Assert.NotNull(block);
        Assert.True(block.Value.Frames >= 480 && block.Value.Frames % 480 == 0);
        AudioMeterReading meter = session.GetInputMeter(a);
        Assert.Equal(2, meter.Channels);
        Assert.InRange(meter.PeakDb[0], -30f, -20f);  // -20 dBFS tone, -6 dB fader, light compression
        Assert.True(session.GetBusMeter(AudioBus.Master).MomentaryLufs < 0);
        session.ResetLoudness(AudioBus.Master);
        SessionStatistics s = session.GetStatistics();
        Assert.True(s.AudioBlocks > 10);
    }
}
