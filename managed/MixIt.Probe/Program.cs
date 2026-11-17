using System.Text.Json;
using System.Text.Json.Serialization;
using Microsoft.Extensions.Logging.Abstractions;
using MixIt.Core.Engine;
using MixIt.Core.Hardware;
using MixIt.Interop;

namespace MixIt.Probe;

/// <summary>Exit codes: 0 success, 1 engine failure, 2 usage error.</summary>
internal static class Program
{
    private static int Main(string[] args)
    {
        bool json = false, verbose = false, codecs = false;
        string? logFile = null;
        int soakSeconds = 0;
        SessionConfiguration soakFormat = new() { Width = 1280, Height = 720, FrameRate = MediaRate.Fps29_97 };
        for (int i = 0; i < args.Length; i++)
        {
            switch (args[i])
            {
                case "--json": json = true; break;
                case "--verbose": verbose = true; break;
                case "--log-file" when i + 1 < args.Length: logFile = args[++i]; break;
                case "--codecs": codecs = true; break;
                case "--session-test" when i + 1 < args.Length && int.TryParse(args[i + 1], out int sec) && sec >= 2:
                    soakSeconds = sec;
                    i++;
                    break;
                case "--format" when i + 1 < args.Length:
                    SessionConfiguration? f = SessionSoak.ParseFormat(args[++i]);
                    if (f is null)
                    {
                        Console.Error.WriteLine("invalid --format, expected e.g. 1920x1080@60000/1001");
                        return 2;
                    }
                    soakFormat = f;
                    break;
                case "-h" or "--help":
                    Console.WriteLine("usage: mixit-probe [--json] [--verbose] [--log-file <path>] [--codecs]");
                    Console.WriteLine("                   [--session-test <seconds> [--format WxH@num[/den]]]");
                    return 0;
                default:
                    Console.Error.WriteLine($"unknown argument: {args[i]}");
                    return 2;
            }
        }

        var options = new EngineHostOptions
        {
            MinimumLogLevel = verbose ? NativeLogLevel.Debug : NativeLogLevel.Information,
            LogFilePath = logFile,
        };

        try
        {
            using var host = new EngineHost(options, NullLogger<EngineHost>.Instance);
            host.Start();
            SystemCapabilities caps = host.DetectCapabilities();
            int soakResult = 0;
            if (soakSeconds > 0)
            {
                soakResult = SessionSoak.Run(host, soakSeconds, soakFormat);
                host.Stop();
                return soakResult;
            }
            host.Stop();  // drains the native log into the stream

            var log = new List<NativeLogEntry>();
            while (host.LogStream.Reader.TryRead(out NativeLogEntry entry)) log.Add(entry);

            if (json)
            {
                var report = new ProbeReport(NativeEngine.LibraryApiVersion.ToString(), NativeEngine.BuildInfo, caps,
                    verbose ? log : null);
                Console.WriteLine(JsonSerializer.Serialize(report, JsonOptions));
            }
            else
            {
                PrintText(caps);
                if (codecs) PrintCodecs();
                if (verbose)
                {
                    Console.WriteLine();
                    Console.WriteLine("Engine log:");
                    foreach (NativeLogEntry e in log)
                    {
                        Console.WriteLine($"  {e.Timestamp:HH:mm:ss.fff} {e.Level,-11} [{e.Category}] {e.Message}");
                    }
                }
            }
            return 0;
        }
        catch (MixItNativeException ex)
        {
            Console.Error.WriteLine($"mixit-probe: {ex.Message}");
            if (ex.InnerException is not null) Console.Error.WriteLine($"  cause: {ex.InnerException.Message}");
            return 1;
        }
    }

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
        Converters = { new JsonStringEnumConverter() },
    };

    private sealed record ProbeReport(string ApiVersion, string BuildInfo, SystemCapabilities Capabilities,
        IReadOnlyList<NativeLogEntry>? Log);

    private static void PrintText(SystemCapabilities c)
    {
        Console.WriteLine("MIX IT hardware probe");
        Console.WriteLine($"  Engine        : {NativeEngine.BuildInfo}");
        Console.WriteLine($"  API version   : library {NativeEngine.LibraryApiVersion}, binding {NativeEngine.BindingApiVersion}");
        Console.WriteLine($"  OS            : {Or(c.OperatingSystem)}");
        Console.WriteLine($"  CPU           : {Or(c.CpuBrand)}");
        Console.WriteLine($"  Cores         : {c.LogicalProcessorCount} logical / {(c.PhysicalCoreCount > 0 ? c.PhysicalCoreCount.ToString() : "unknown")} physical");
        Console.WriteLine($"  CPU features  : {CapabilityFormatting.FormatCpuFeatures(c.CpuFeatures)}");
        Console.WriteLine($"  Memory        : {CapabilityFormatting.FormatBytes(c.AvailablePhysicalMemoryBytes)} available of {CapabilityFormatting.FormatBytes(c.TotalPhysicalMemoryBytes)}");
        Console.WriteLine($"  GPU           : {CapabilityFormatting.DescribeGpuAvailability(c)}");
        foreach (GpuAdapterInfo g in c.GpuAdapters)
        {
            Console.WriteLine($"    - {g.Description} [{g.VendorName} 0x{g.DeviceId:X4}] VRAM {CapabilityFormatting.FormatBytes(g.DedicatedVideoMemoryBytes)}{(g.IsSoftware ? " (software)" : "")}");
        }
        Console.WriteLine($"  Detected      : {c.Detected}");
    }

    private static readonly string[] CodecsOfInterest =
    [
        "h264", "hevc", "aac", "libx264", "libx265", "libopenh264",
        "h264_nvenc", "hevc_nvenc", "h264_qsv", "hevc_qsv", "h264_amf", "hevc_amf", "prores_ks",
    ];

    private static void PrintCodecs()
    {
        MediaLibraryInfo m = NativeEngine.GetMediaLibraryInfo();
        Console.WriteLine();
        if (!m.CompiledIn)
        {
            Console.WriteLine("  FFmpeg        : not built in");
            return;
        }
        string verdict = m.Usable
            ? (m.LicenseIsLgpl ? "usable (LGPL)" : "usable on this DEV build only (not LGPL - must not be distributed)")
            : !m.VersionsMatch ? "NOT usable (runtime/header version mismatch)" : "NOT usable (rejected by LGPL policy)";
        Console.WriteLine($"  FFmpeg        : {m.Version} (avcodec {m.AvcodecVersion}), licence '{m.License}'");
        Console.WriteLine($"  Status        : {verdict}");
        if (!m.Usable) return;
        Console.WriteLine("  Codecs (present in build; hardware encoders are not yet open-tested):");
        foreach (string name in CodecsOfInterest)
        {
            CodecInfo? c = NativeEngine.QueryCodec(name);
            string what = c is null ? "-" : $"{(c.Decoder ? "dec " : "")}{(c.Encoder ? "enc " : "")}{(c.Hardware ? "hw" : "")}".Trim();
            Console.WriteLine($"    {name,-12} {what}");
        }
    }

    private static string Or(string s) => string.IsNullOrWhiteSpace(s) ? "unknown" : s;
}
