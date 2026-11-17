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
}
