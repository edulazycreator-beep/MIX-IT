using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using MixIt.Interop;
using Xunit;

namespace MixIt.Tests;

/// <summary>
/// The managed struct mirrors must match the C header byte for byte. The
/// native side asserts the same numbers with static_assert in engine_abi.cpp.
/// </summary>
public class InteropLayoutTests
{
    [Fact]
    public void StructSizesMatchNativeAbi()
    {
        Assert.Equal(NativeStructSizes.EngineConfig, Unsafe.SizeOf<MixitEngineConfig>());
        Assert.Equal(NativeStructSizes.LogRecord, Unsafe.SizeOf<MixitLogRecord>());
        Assert.Equal(NativeStructSizes.EngineDiagnostics, Unsafe.SizeOf<MixitEngineDiagnostics>());
        Assert.Equal(NativeStructSizes.SystemCapabilities, Unsafe.SizeOf<MixitSystemCapabilities>());
        Assert.Equal(NativeStructSizes.GpuAdapterInfo, Unsafe.SizeOf<MixitGpuAdapterInfo>());
        Assert.Equal(NativeStructSizes.SessionConfig, Unsafe.SizeOf<MixitSessionConfig>());
        Assert.Equal(NativeStructSizes.TestSourceConfig, Unsafe.SizeOf<MixitTestSourceConfig>());
        Assert.Equal(NativeStructSizes.SessionStats, Unsafe.SizeOf<MixitSessionStats>());
        Assert.Equal(NativeStructSizes.InputStats, Unsafe.SizeOf<MixitInputStats>());
        Assert.Equal(NativeStructSizes.VideoFrameInfo, Unsafe.SizeOf<MixitVideoFrameInfo>());
        Assert.Equal(NativeStructSizes.MediaLibraryInfo, Unsafe.SizeOf<MixitMediaLibraryInfo>());
        Assert.Equal(NativeStructSizes.CodecInfo, Unsafe.SizeOf<MixitCodecInfo>());
        Assert.Equal(NativeStructSizes.FileSourceConfig, Unsafe.SizeOf<MixitFileSourceConfig>());
    }

    [Fact]
    public void FieldOffsetsMatchNativeAbi()
    {
        Assert.Equal(16, (int)Marshal.OffsetOf<MixitEngineConfig>(nameof(MixitEngineConfig.LogFilePath)));
        Assert.Equal(24, (int)Marshal.OffsetOf<MixitEngineConfig>(nameof(MixitEngineConfig.LogCallback)));
        Assert.Equal(32, (int)Marshal.OffsetOf<MixitEngineConfig>(nameof(MixitEngineConfig.LogUserData)));
        Assert.Equal(8, (int)Marshal.OffsetOf<MixitLogRecord>(nameof(MixitLogRecord.UnixTimeNs)));
        Assert.Equal(40, (int)Marshal.OffsetOf<MixitLogRecord>(nameof(MixitLogRecord.Message)));
        Assert.Equal(24, (int)Marshal.OffsetOf<MixitSystemCapabilities>(nameof(MixitSystemCapabilities.TotalPhysicalMemoryBytes)));
        Assert.Equal(40, (int)Marshal.OffsetOf<MixitSystemCapabilities>(nameof(MixitSystemCapabilities.CpuBrand)));
        Assert.Equal(104, (int)Marshal.OffsetOf<MixitSystemCapabilities>(nameof(MixitSystemCapabilities.OsDescription)));
        Assert.Equal(32, (int)Marshal.OffsetOf<MixitGpuAdapterInfo>(nameof(MixitGpuAdapterInfo.Description)));
        Assert.Equal(16, (int)Marshal.OffsetOf<MixitSessionStats>(nameof(MixitSessionStats.Ticks)));
        Assert.Equal(88, (int)Marshal.OffsetOf<MixitSessionStats>(nameof(MixitSessionStats.VideoLatencyMaxNs)));
        Assert.Equal(8, (int)Marshal.OffsetOf<MixitInputStats>(nameof(MixitInputStats.FramesProduced)));
        Assert.Equal(104, (int)Marshal.OffsetOf<MixitInputStats>(nameof(MixitInputStats.AvOffsetMaxAbsNs)));
        Assert.Equal(112, (int)Marshal.OffsetOf<MixitInputStats>(nameof(MixitInputStats.DecodeErrors)));
        Assert.Equal(24, (int)Marshal.OffsetOf<MixitVideoFrameInfo>(nameof(MixitVideoFrameInfo.TickPtsNs)));
        Assert.Equal(48, (int)Marshal.OffsetOf<MixitVideoFrameInfo>(nameof(MixitVideoFrameInfo.TickIndex)));
    }

    [Fact]
    public void EnumValuesMatchNativeConstants()
    {
        Assert.Equal(-2, (int)NativeResult.InvalidHandle);
        Assert.Equal(-10, (int)NativeResult.Io);
        Assert.Equal(-99, (int)NativeResult.Internal);
        Assert.Equal(6, (int)NativeLogLevel.None);
        Assert.Equal(2, (int)EngineState.ShutDown);
        Assert.Equal(1u << 6, (uint)CpuFeatures.Avx512F);
        Assert.Equal(1u << 3, (uint)DetectedComponents.GpuAdapters);
    }
}
