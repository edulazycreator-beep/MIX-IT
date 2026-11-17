using MixIt.Core.Hardware;
using MixIt.Interop;
using Xunit;

namespace MixIt.Tests;

public class CapabilityFormattingTests
{
    [Fact]
    public void FormatsBytes()
    {
        Assert.Equal("512 B", CapabilityFormatting.FormatBytes(512));
        Assert.Equal("1.0 KiB", CapabilityFormatting.FormatBytes(1024));
        Assert.Equal("16.0 GiB", CapabilityFormatting.FormatBytes(16UL * 1024 * 1024 * 1024));
    }

    [Fact]
    public void FormatsCpuFeatures()
    {
        Assert.Equal("none detected", CapabilityFormatting.FormatCpuFeatures(CpuFeatures.None));
        Assert.Equal("SSE2 AVX2", CapabilityFormatting.FormatCpuFeatures(CpuFeatures.Sse2 | CpuFeatures.Avx2));
    }

    [Fact]
    public void DistinguishesUnavailableGpuEnumerationFromNoAdapters()
    {
        var notProbed = new SystemCapabilities(DetectedComponents.Cpu, 1, 1, CpuFeatures.None, 1, 1, "", "", []);
        var probedNone = notProbed with { Detected = DetectedComponents.Cpu | DetectedComponents.GpuAdapters };
        var probedOne = probedNone with
        {
            GpuAdapters = [new GpuAdapterInfo(0x10DE, 0x2684, GpuAdapterFlags.None, 1, 0, "Test GPU")],
        };
        Assert.Contains("unavailable", CapabilityFormatting.DescribeGpuAvailability(notProbed));
        Assert.Equal("No GPU adapters reported", CapabilityFormatting.DescribeGpuAvailability(probedNone));
        Assert.Equal("1 adapter(s)", CapabilityFormatting.DescribeGpuAvailability(probedOne));
        Assert.Equal("NVIDIA", probedOne.GpuAdapters[0].VendorName);
    }
}
