using System.Globalization;
using MixIt.Interop;

namespace MixIt.Core.Hardware;

/// <summary>Presentation helpers for capability data (shared by the UI and the probe tool).</summary>
public static class CapabilityFormatting
{
    /// <summary>Formats a byte count as e.g. "15.9 GiB".</summary>
    public static string FormatBytes(ulong bytes)
    {
        string[] units = ["B", "KiB", "MiB", "GiB", "TiB"];
        double value = bytes;
        int unit = 0;
        while (value >= 1024 && unit < units.Length - 1)
        {
            value /= 1024;
            unit++;
        }
        return unit == 0
            ? string.Create(CultureInfo.InvariantCulture, $"{bytes} B")
            : string.Create(CultureInfo.InvariantCulture, $"{value:0.0} {units[unit]}");
    }

    /// <summary>Lists set CPU feature flags, e.g. "SSE2 SSE4.2 AVX AVX2".</summary>
    public static string FormatCpuFeatures(CpuFeatures features)
    {
        if (features == CpuFeatures.None) return "none detected";
        var names = new List<string>();
        if (features.HasFlag(CpuFeatures.Sse2)) names.Add("SSE2");
        if (features.HasFlag(CpuFeatures.Sse41)) names.Add("SSE4.1");
        if (features.HasFlag(CpuFeatures.Sse42)) names.Add("SSE4.2");
        if (features.HasFlag(CpuFeatures.Avx)) names.Add("AVX");
        if (features.HasFlag(CpuFeatures.Avx2)) names.Add("AVX2");
        if (features.HasFlag(CpuFeatures.Fma3)) names.Add("FMA3");
        if (features.HasFlag(CpuFeatures.Avx512F)) names.Add("AVX-512F");
        return string.Join(' ', names);
    }

    /// <summary>
    /// Text for the GPU section that distinguishes "no adapters" from
    /// "enumeration unavailable" — the UI must never imply hardware is absent
    /// when it simply was not probed.
    /// </summary>
    public static string DescribeGpuAvailability(SystemCapabilities caps) =>
        !caps.GpuEnumerationAvailable
            ? "GPU enumeration unavailable on this platform/session"
            : caps.GpuAdapters.Count == 0
                ? "No GPU adapters reported"
                : $"{caps.GpuAdapters.Count} adapter(s)";
}
