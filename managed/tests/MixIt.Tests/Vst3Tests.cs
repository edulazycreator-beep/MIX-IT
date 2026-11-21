using System.Reflection;
using MixIt.Interop;
using Xunit;

namespace MixIt.Tests;

public sealed class Vst3Tests
{
    /// <summary>The VST3 test plug-in built with the native tests, or null when it was not built.</summary>
    internal static string? TestBundle()
    {
        string? dir = typeof(Vst3Tests).Assembly.GetCustomAttributes<AssemblyMetadataAttribute>()
            .FirstOrDefault(a => a.Key == "MixItNativeDir")?.Value;
        for (int up = 0; dir is not null && up < 4; up++, dir = Path.GetDirectoryName(dir))
        {
            string candidate = Path.Combine(dir, "vst3", "MixItTestGain.vst3");
            if (Directory.Exists(candidate)) return candidate;
        }
        if (Environment.GetEnvironmentVariable("MIXIT_REQUIRE_VST3_TEST_BUNDLE") == "1")
        {
            throw new InvalidOperationException("MixItTestGain.vst3 not found next to the native engine (CI requires it).");
        }
        return null;
    }

    [Fact]
    public void ScanInsertParametersBypassAndState()
    {
        string? bundle = TestBundle();
        if (bundle is null) return;  // native tests (and the test plug-in) not built next to this engine

        using var engine = NativeEngine.Create(new NativeEngineOptions());
        Vst3Plugin plugin = Assert.Single(engine.ScanVst3(bundle));
        Assert.Equal("MIX IT Test Gain", plugin.Name);
        Assert.Equal("MIX IT Test Gain (MIX IT tests)", plugin.DisplayName);

        using NativeSession session = engine.CreateSession(new SessionConfiguration
        {
            Width = 320, Height = 180, FrameRate = MediaRate.Fps30, AudioSampleRate = 48000, AudioChannels = 2,
        });
        uint a = session.AddTestSource(new TestSourceOptions { Width = 320, Height = 180, FrameRate = MediaRate.Fps30, ToneHz = 1000 });
        Assert.Null(session.GetAudioInsert(a, 0));

        session.SetAudioInsert(a, 0, plugin.Path, plugin.ClassId);
        AudioInsert insert = session.GetAudioInsert(a, 0)!;
        Assert.Equal("MIX IT Test Gain", insert.Name);
        Assert.False(insert.Bypass);

        IReadOnlyList<AudioInsertParameter> parameters = session.GetAudioInsertParameters(a, 0);
        Assert.Equal(2, parameters.Count);
        Assert.Equal("Gain", parameters[0].Title);
        Assert.Equal("1.00", parameters[0].Display);
        Assert.True(parameters[1].IsBypass);

        session.SetAudioInsertParameter(a, 0, parameters[0].Id, 0.25);
        Assert.Equal("0.50", session.GetAudioInsertParameters(a, 0)[0].Display);
        session.SetAudioInsertBypass(a, 0, true);
        Assert.True(session.GetAudioInsert(a, 0)!.Bypass);

        byte[] state = session.GetAudioInsertState(a, 0);
        session.SetAudioInsert(a, 1, plugin.Path, plugin.ClassId, state);
        Assert.NotNull(session.GetAudioInsert(a, 1));

        session.RemoveAudioInsert(a, 0);
        Assert.Null(session.GetAudioInsert(a, 0));
        var ex = Assert.Throws<MixItNativeException>(() => session.RemoveAudioInsert(a, 0));
        Assert.Equal(NativeResult.NotFound, ex.Result);
        Assert.Throws<MixItNativeException>(() => session.SetAudioInsert(a, 2, plugin.Path, plugin.ClassId));
    }
}

public sealed class UrlRedactionTests
{
    [Fact]
    public void HidesSecrets()
    {
        Assert.Equal("rtmp://a.rtmp.youtube.com/live2/***", UrlRedaction.Redact("rtmp://a.rtmp.youtube.com/live2/abcd-efgh"));
        Assert.Equal("srt://host:9000?mode=***&passphrase=***", UrlRedaction.Redact("srt://host:9000?mode=caller&passphrase=secret"));
        Assert.Equal("rtmp://***@host/app/***", UrlRedaction.Redact("rtmp://user:pass@host/app/key"));
        Assert.Equal(@"C:\Recordings\show.mp4", UrlRedaction.Redact(@"C:\Recordings\show.mp4"));
    }
}
