using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;
using MixIt.Core.Engine;
using MixIt.Interop;
using Xunit;

namespace MixIt.Tests;

public class EngineHostTests
{
    private static EngineHost NewHost(EngineHostOptions? options = null) =>
        new(options ?? new EngineHostOptions(), NullLogger<EngineHost>.Instance);

    private static List<NativeLogEntry> Drain(EngineHost host)
    {
        var list = new List<NativeLogEntry>();
        while (host.LogStream.Reader.TryRead(out NativeLogEntry e)) list.Add(e);
        return list;
    }

    [Fact]
    public void StartAndStopRaiseStateChangesAndAreIdempotent()
    {
        using EngineHost host = NewHost();
        var states = new List<EngineHostState>();
        host.StateChanged += (_, s) => states.Add(s);

        Assert.Equal(EngineHostState.Stopped, host.State);
        host.Start();
        host.Start();
        Assert.Equal(EngineHostState.Running, host.State);
        Assert.NotNull(host.GetDiagnostics());
        host.Stop();
        host.Stop();
        Assert.Equal(EngineHostState.Stopped, host.State);
        Assert.Null(host.GetDiagnostics());
        Assert.Equal(new[] { EngineHostState.Running, EngineHostState.Stopped }, states);
    }

    [Fact]
    public void CanRestartAfterStop()
    {
        using EngineHost host = NewHost();
        host.Start();
        host.Stop();
        host.Start();
        Assert.Equal(EngineHostState.Running, host.State);
        Assert.NotNull(host.DetectCapabilities());
    }

    [Fact]
    public void DetectCapabilitiesRequiresRunningEngineAndCaches()
    {
        using EngineHost host = NewHost();
        Assert.Throws<InvalidOperationException>(() => host.DetectCapabilities());
        Assert.Null(host.Capabilities);
        host.Start();
        SystemCapabilities caps = host.DetectCapabilities();
        Assert.Same(caps, host.Capabilities);
    }

    [Fact]
    public void StartFailureFaultsTheHost()
    {
        string badPath = Path.Combine(Path.GetTempPath(), "mixit-missing-" + Guid.NewGuid(), "x.jsonl");
        using EngineHost host = NewHost(new EngineHostOptions { LogFilePath = badPath });
        var states = new List<EngineHostState>();
        host.StateChanged += (_, s) => states.Add(s);

        var ex = Assert.Throws<MixItNativeException>(() => host.Start());
        Assert.Equal(NativeResult.Io, ex.Result);
        Assert.Equal(EngineHostState.Faulted, host.State);
        Assert.Same(ex, host.LastError);
        Assert.Equal(new[] { EngineHostState.Faulted }, states);
        host.Stop();
        Assert.Equal(EngineHostState.Stopped, host.State);
    }

    [Fact]
    public void LogStreamReceivesEngineRecords()
    {
        using EngineHost host = NewHost();
        host.Start();
        host.DetectCapabilities();
        host.Stop();
        List<NativeLogEntry> entries = Drain(host);
        Assert.Contains(entries, e => e.Category == "engine" && e.Message.StartsWith("engine initialized", StringComparison.Ordinal));
        Assert.Contains(entries, e => e.Category == "caps" && e.Message.StartsWith("detected:", StringComparison.Ordinal));
    }

    [Fact]
    public void LogStreamDropsOldestWhenConsumerFallsBehind()
    {
        using EngineHost host = NewHost(new EngineHostOptions { LogStreamCapacity = 4 });
        host.Start();
        for (int i = 0; i < 50; i++) host.TryForwardLog(NativeLogLevel.Information, "flood", $"m{i}");
        host.Stop();
        List<NativeLogEntry> entries = Drain(host);
        Assert.True(entries.Count <= 4, $"stream exceeded its bound: {entries.Count}");
        Assert.True(host.LogStream.DroppedCount > 0);
        Assert.Equal("engine", entries[^1].Category);  // newest (shutdown) record survived
    }

    [Fact]
    public void ForwardingProviderWritesManagedLogsIntoEngineStream()
    {
        using EngineHost host = NewHost(new EngineHostOptions { MinimumLogLevel = NativeLogLevel.Debug });
        using var provider = new NativeEngineLoggerProvider(host);
        ILogger logger = provider.CreateLogger("Desktop.Test");

        Assert.False(logger.IsEnabled(LogLevel.Information), "disabled while the engine is stopped");
        host.Start();
        Assert.True(logger.IsEnabled(LogLevel.Information));
        logger.LogWarning("operator pressed {Button} at {Count}", "CUT", 3);
        logger.LogError(new InvalidOperationException("boom"), "failure");
        host.Stop();

        List<NativeLogEntry> entries = Drain(host).Where(e => e.Category == "Desktop.Test").ToList();
        Assert.Equal(2, entries.Count);
        Assert.Equal(NativeLogLevel.Warning, entries[0].Level);
        Assert.Equal("operator pressed CUT at 3", entries[0].Message);
        Assert.Equal(NativeLogLevel.Error, entries[1].Level);
        Assert.Contains("InvalidOperationException: boom", entries[1].Message);
    }

    [Fact]
    public void LogLevelMappingIsOneToOne()
    {
        Assert.Equal(NativeLogLevel.Trace, NativeEngineLoggerProvider.Map(LogLevel.Trace));
        Assert.Equal(NativeLogLevel.Information, NativeEngineLoggerProvider.Map(LogLevel.Information));
        Assert.Equal(NativeLogLevel.Critical, NativeEngineLoggerProvider.Map(LogLevel.Critical));
        Assert.Equal(NativeLogLevel.None, NativeEngineLoggerProvider.Map(LogLevel.None));
    }
}
