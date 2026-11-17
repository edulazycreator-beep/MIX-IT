using System.Collections.Concurrent;
using System.Runtime.CompilerServices;
using MixIt.Interop;
using Xunit;

namespace MixIt.Tests;

/// <summary>Integration tests of the interop layer against the real native engine.</summary>
public class NativeEngineTests
{
    [Fact]
    public void LibraryApiVersionMatchesBinding()
    {
        Assert.Equal(NativeEngine.BindingApiVersion, NativeEngine.LibraryApiVersion);
        NativeEngine.EnsureCompatibleLibrary();
        Assert.Contains("MIX IT native engine", NativeEngine.BuildInfo);
    }

    [Fact]
    public void MonotonicClockAdvances()
    {
        ulong a = NativeEngine.MonotonicTimeNanoseconds;
        Thread.Sleep(5);
        ulong b = NativeEngine.MonotonicTimeNanoseconds;
        Assert.True(b - a >= 1_000_000, $"clock advanced only {b - a} ns");
    }

    [Fact]
    public void LifecycleCreateShutdownDispose()
    {
        var engine = NativeEngine.Create(new NativeEngineOptions());
        Assert.Equal(EngineState.Initialized, engine.GetDiagnostics().State);

        engine.Shutdown();
        engine.Shutdown();  // idempotent
        EngineDiagnostics d = engine.GetDiagnostics();
        Assert.Equal(EngineState.ShutDown, d.State);
        Assert.Equal(d.LogRecordsAccepted, d.LogRecordsDelivered);

        engine.Dispose();
        engine.Dispose();  // idempotent
        Assert.Throws<ObjectDisposedException>(() => engine.GetDiagnostics());
        Assert.False(engine.TryLog(NativeLogLevel.Error, "t", "after dispose"));
    }

    [Fact]
    public void OperationsAfterShutdownReportInvalidState()
    {
        using var engine = NativeEngine.Create(new NativeEngineOptions());
        engine.Shutdown();
        var ex = Assert.Throws<MixItNativeException>(() => engine.DetectCapabilities());
        Assert.Equal(NativeResult.InvalidState, ex.Result);
        Assert.Equal("mixit_engine_detect_capabilities", ex.Function);
        Assert.Contains("initialized", ex.NativeMessage);
        Assert.False(engine.TryLog(NativeLogLevel.Information, "t", "m"));
    }

    [Fact]
    public void UnopenableLogFileFailsWithIoError()
    {
        string path = Path.Combine(Path.GetTempPath(), "mixit-no-such-dir-" + Guid.NewGuid(), "log.jsonl");
        var ex = Assert.Throws<MixItNativeException>(() => NativeEngine.Create(new NativeEngineOptions { LogFilePath = path }));
        Assert.Equal(NativeResult.Io, ex.Result);
        Assert.Contains("cannot open log file", ex.Message);
    }

    [Fact]
    public void LogHandlerReceivesHostRecordsInOrder()
    {
        var received = new ConcurrentQueue<NativeLogEntry>();
        using (var engine = NativeEngine.Create(new NativeEngineOptions
               {
                   MinimumLogLevel = NativeLogLevel.Debug,
                   LogHandler = received.Enqueue,
               }))
        {
            for (int i = 0; i < 100; i++)
            {
                Assert.True(engine.TryLog(NativeLogLevel.Debug, "test", $"record {i} é中"));
            }
            engine.Shutdown();  // drains every accepted record before returning
        }

        NativeLogEntry[] host = received.Where(e => e.Category == "test").ToArray();
        Assert.Equal(100, host.Length);
        for (int i = 0; i < host.Length; i++)
        {
            Assert.Equal($"record {i} é中", host[i].Message);
            Assert.Equal(NativeLogLevel.Debug, host[i].Level);
        }
        ulong[] sequences = received.Select(e => e.Sequence).ToArray();
        Assert.True(sequences.SequenceEqual(sequences.Order()), "records must arrive in sequence order");
        Assert.InRange(host[0].Timestamp, DateTimeOffset.UtcNow.AddMinutes(-5), DateTimeOffset.UtcNow.AddMinutes(1));
    }

    [Fact]
    public void LogHandlerExceptionsAreContainedAndCounted()
    {
        using var engine = NativeEngine.Create(new NativeEngineOptions
        {
            LogHandler = _ => throw new InvalidOperationException("handler bug"),
        });
        for (int i = 0; i < 10; i++) engine.TryLog(NativeLogLevel.Warning, "t", "m");
        engine.Shutdown();
        Assert.True(engine.LogHandlerFailures >= 10, $"failures={engine.LogHandlerFailures}");
    }

    [Fact]
    public void DisposingFromLogHandlerIsRejected()
    {
        var holder = new StrongBox<NativeEngine?>();
        Exception? observed = null;
        using var gate = new ManualResetEventSlim();
        var engine = NativeEngine.Create(new NativeEngineOptions
        {
            LogHandler = e =>
            {
                if (e.Message != "trigger") return;
                try { holder.Value!.Dispose(); }
                catch (Exception ex) { observed = ex; }
                gate.Set();
            },
        });
        holder.Value = engine;
        engine.TryLog(NativeLogLevel.Information, "t", "trigger");
        Assert.True(gate.Wait(TimeSpan.FromSeconds(10)), "log handler was not invoked");
        Assert.IsType<InvalidOperationException>(observed);
        engine.Dispose();  // owner can still dispose normally
    }

    [Fact]
    public void DetectCapabilitiesReturnsPlausibleValues()
    {
        using var engine = NativeEngine.Create(new NativeEngineOptions());
        SystemCapabilities caps = engine.DetectCapabilities();
        Assert.True(caps.Detected.HasFlag(DetectedComponents.Cpu));
        Assert.True(caps.Detected.HasFlag(DetectedComponents.Memory));
        Assert.True(caps.LogicalProcessorCount > 0);
        Assert.True(caps.TotalPhysicalMemoryBytes > 0);
        Assert.True(caps.AvailablePhysicalMemoryBytes <= caps.TotalPhysicalMemoryBytes);
        if (!caps.GpuEnumerationAvailable) Assert.Empty(caps.GpuAdapters);
        foreach (GpuAdapterInfo gpu in caps.GpuAdapters) Assert.False(string.IsNullOrEmpty(gpu.Description));
    }

    [Fact]
    public void FinalizerReleasesAnUndisposedEngine()
    {
        var received = new ConcurrentQueue<string>();
        CreateAndAbandon(received);
        for (int i = 0; i < 50 && !received.Contains("engine-shutdown-seen"); i++)
        {
            GC.Collect();
            GC.WaitForPendingFinalizers();
            if (received.Any(m => m.StartsWith("engine shutting down", StringComparison.Ordinal)))
            {
                received.Enqueue("engine-shutdown-seen");
            }
            Thread.Sleep(10);
        }
        Assert.Contains("engine-shutdown-seen", received);
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static void CreateAndAbandon(ConcurrentQueue<string> sink)
    {
        _ = NativeEngine.Create(new NativeEngineOptions { LogHandler = e => sink.Enqueue(e.Message) });
    }

    [Fact]
    public void ConcurrentLoggingWhileDisposingIsSafe()
    {
        for (int round = 0; round < 10; round++)
        {
            var engine = NativeEngine.Create(new NativeEngineOptions { MinimumLogLevel = NativeLogLevel.Trace });
            using var start = new ManualResetEventSlim();
            var errors = new ConcurrentQueue<Exception>();
            Thread[] workers = Enumerable.Range(0, 4).Select(_ => new Thread(() =>
            {
                start.Wait();
                for (int i = 0; i < 2000; i++)
                {
                    try { engine.TryLog(NativeLogLevel.Trace, "race", "x"); }
                    catch (Exception ex) { errors.Enqueue(ex); }
                }
            })).ToArray();
            foreach (Thread t in workers) t.Start();
            start.Set();
            Thread.Sleep(1);
            engine.Dispose();
            foreach (Thread t in workers) t.Join();
            Assert.Empty(errors);
        }
    }
}
