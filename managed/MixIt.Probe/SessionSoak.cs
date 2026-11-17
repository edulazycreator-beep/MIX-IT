using System.Diagnostics;
using MixIt.Core.Engine;
using MixIt.Interop;

namespace MixIt.Probe;

/// <summary>
/// Runs a synthetic production session for a fixed time and judges it
/// against the M2 pipeline criteria. Exit code 0 = PASS, 1 = FAIL.
/// </summary>
internal static class SessionSoak
{
    // Half a sample period at 48 kHz: the best achievable flash/click alignment.
    private const long MaxAvOffsetNs = 10_417;

    public static int Run(EngineHost host, int seconds, SessionConfiguration format)
    {
        using NativeSession session = host.CreateSession(format);
        uint main = session.AddTestSource(new TestSourceOptions
        {
            Width = format.Width, Height = format.Height, FrameRate = format.FrameRate,
            FlashIntervalFrames = (uint)Math.Max(1, Math.Round(format.FrameRate.Value)),
        });
        uint second = session.AddTestSource(new TestSourceOptions
        {
            Width = Math.Max(64, format.Width / 2 / 2 * 2), Height = Math.Max(64, format.Height / 2 / 2 * 2),
            FrameRate = MediaRate.Fps25, ToneHz = 440,
        });
        session.SetProgramInput(main);

        Console.WriteLine($"Session soak: {format.Width}x{format.Height} @ {format.FrameRate} fps for {seconds} s " +
                          $"(input {main}: same format + sync flash; input {second}: half size @ 25 fps)");
        Console.WriteLine("   t  ticks  late  black  latency(ms)  in1 drop/rep/skip  in2 rep/skip  A/V max(us)");

        session.Start();
        var clock = Stopwatch.StartNew();
        for (int t = 1; t <= seconds; t++)
        {
            Thread.Sleep(TimeSpan.FromSeconds(t) - clock.Elapsed);
            if (t == seconds / 2) session.SetProgramInput(second);       // cut
            if (t == seconds / 2 + 1) session.SetProgramInput(main);     // and back
            SessionStatistics s = session.GetStatistics();
            InputStatistics a = session.GetInputStatistics(main);
            InputStatistics b = session.GetInputStatistics(second);
            Console.WriteLine($"{t,4} {s.Ticks,6} {s.LateTicks,5} {s.ProgramBlackFrames,6} {s.VideoLatencyLast.TotalMilliseconds,12:0.0}" +
                              $"  {a.FramesDropped,6}/{a.FramesRepeated}/{a.FramesSkipped,-6} {b.FramesRepeated,6}/{b.FramesSkipped,-5}" +
                              $" {a.AvOffsetMaxAbsNanoseconds / 1000.0,10:0.0}");
        }
        session.Stop();

        SessionStatistics fs = session.GetStatistics();
        InputStatistics fa = session.GetInputStatistics(main);
        InputStatistics fb = session.GetInputStatistics(second);
        double expectedTicks = clock.Elapsed.TotalSeconds * format.FrameRate.Value;

        var failures = new List<string>();
        if (fs.Ticks < expectedTicks * 0.95) failures.Add($"only {fs.Ticks} ticks, expected ~{expectedTicks:0}");
        if (fs.LateTicks > fs.Ticks / 100) failures.Add($"{fs.LateTicks} late ticks (> 1%)");
        if (fs.ProgramPoolExhausted + fa.FramesPoolExhausted + fb.FramesPoolExhausted > 0) failures.Add("frame pool exhausted");
        if (fa.FramesDropped > 0) failures.Add($"{fa.FramesDropped} frames dropped on input {main}");
        if (fa.AudioBlocksDropped + fb.AudioBlocksDropped > 0) failures.Add("audio blocks dropped");
        if (fa.AvSyncEvents < (ulong)(seconds / 2)) failures.Add($"only {fa.AvSyncEvents} A/V sync events measured");
        if (fa.AvOffsetMaxAbsNanoseconds > MaxAvOffsetNs) failures.Add($"A/V offset {fa.AvOffsetMaxAbsNanoseconds} ns > {MaxAvOffsetNs} ns");

        Console.WriteLine();
        Console.WriteLine($"ticks {fs.Ticks} (expected ~{expectedTicks:0}), late {fs.LateTicks}, skipped {fs.SkippedTicks}, " +
                          $"max lateness {fs.MaxTickLateness.TotalMilliseconds:0.00} ms, max video latency {fs.VideoLatencyMax.TotalMilliseconds:0.0} ms");
        Console.WriteLine($"input {main}: produced {fa.FramesProduced}, presented {fa.FramesPresented}, A/V events {fa.AvSyncEvents}, " +
                          $"max |offset| {fa.AvOffsetMaxAbsNanoseconds} ns, audio blocks {fa.AudioBlocksConsumed}");
        Console.WriteLine($"input {second}: produced {fb.FramesProduced}, presented {fb.FramesPresented}, repeated {fb.FramesRepeated}");
        if (failures.Count == 0)
        {
            Console.WriteLine("RESULT: PASS");
            return 0;
        }
        Console.WriteLine("RESULT: FAIL");
        foreach (string f in failures) Console.WriteLine($"  - {f}");
        return 1;
    }

    /// <summary>Parses "1920x1080@60000/1001" or "1280x720@30".</summary>
    public static SessionConfiguration? ParseFormat(string text)
    {
        string[] at = text.Split('@');
        if (at.Length != 2) return null;
        string[] size = at[0].Split('x');
        string[] rate = at[1].Split('/');
        if (size.Length != 2 || !uint.TryParse(size[0], out uint w) || !uint.TryParse(size[1], out uint h)) return null;
        if (!uint.TryParse(rate[0], out uint num)) return null;
        uint den = 1;
        if (rate.Length == 2 && !uint.TryParse(rate[1], out den)) return null;
        return new SessionConfiguration { Width = w, Height = h, FrameRate = new MediaRate(num, den) };
    }
}
