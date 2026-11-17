using System.Runtime.InteropServices;

namespace MixIt.Interop;

// Blittable mirrors of the structs in native/include/mixit/mixit_engine.h.
// Field order, types and sizes MUST match the header exactly. Sizes are
// asserted on both sides (static_assert in engine_abi.cpp and
// InteropLayoutTests in MixIt.Tests).

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct MixitEngineConfig
{
    public uint StructSize;
    public uint ApiVersion;
    public int MinLogLevel;
    public uint LogQueueCapacity;
    public byte* LogFilePath;
    public delegate* unmanaged[Cdecl]<MixitLogRecord*, void*, void> LogCallback;
    public void* LogUserData;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct MixitLogRecord
{
    public uint StructSize;
    public int Level;
    public long UnixTimeNs;
    public ulong ThreadId;
    public ulong Sequence;
    public byte* Category;
    public byte* Message;
}

[StructLayout(LayoutKind.Sequential)]
internal struct MixitEngineDiagnostics
{
    public uint StructSize;
    public int State;
    public ulong UptimeNs;
    public ulong LogRecordsAccepted;
    public ulong LogRecordsDropped;
    public ulong LogRecordsDelivered;
    public ulong LogFileWriteErrors;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct MixitSystemCapabilities
{
    public uint StructSize;
    public uint DetectionFlags;
    public uint LogicalProcessorCount;
    public uint PhysicalCoreCount;
    public uint CpuFeatures;
    public uint GpuAdapterCount;
    public ulong TotalPhysicalMemoryBytes;
    public ulong AvailablePhysicalMemoryBytes;
    public fixed byte CpuBrand[64];
    public fixed byte OsDescription[128];
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct MixitGpuAdapterInfo
{
    public uint StructSize;
    public uint VendorId;
    public uint DeviceId;
    public uint Flags;
    public ulong DedicatedVideoMemoryBytes;
    public ulong SharedSystemMemoryBytes;
    public fixed byte Description[128];
}

[StructLayout(LayoutKind.Sequential)]
internal struct MixitSessionConfig
{
    public uint StructSize;
    public uint Width;
    public uint Height;
    public uint FrameRateNum;
    public uint FrameRateDen;
    public uint AudioSampleRate;
    public uint AudioChannels;
    public uint ProgramPoolFrames;
}

[StructLayout(LayoutKind.Sequential)]
internal struct MixitTestSourceConfig
{
    public uint StructSize;
    public uint Width;
    public uint Height;
    public uint FrameRateNum;
    public uint FrameRateDen;
    public uint ToneHz;
    public uint FlashIntervalFrames;
    public uint Reserved;
}

[StructLayout(LayoutKind.Sequential)]
internal struct MixitSessionStats
{
    public uint StructSize;
    public int State;
    public uint InputCount;
    public uint ProgramInputId;
    public ulong Ticks;
    public ulong LateTicks;
    public ulong SkippedTicks;
    public long MaxTickLatenessNs;
    public ulong ProgramFrames;
    public ulong ProgramBlackFrames;
    public ulong ProgramFramesDropped;
    public ulong ProgramPoolExhausted;
    public long VideoLatencyLastNs;
    public long VideoLatencyMaxNs;
    public uint PreviewInputId;          // API 0.3
    public int TransitionActive;         // API 0.3: MIXIT_TRANSITION_* or -1
    public float TransitionProgress;     // API 0.3
    public int Compositor;               // API 0.3: MIXIT_COMPOSITOR_*
    public ulong TransitionsCompleted;   // API 0.3
    public ulong CompositedFrames;       // API 0.3
}

[StructLayout(LayoutKind.Sequential)]
internal struct MixitLayerConfig
{
    public uint StructSize;
    public uint InputId;
    public float CropX;
    public float CropY;
    public float CropW;
    public float CropH;
    public float DestX;
    public float DestY;
    public float DestW;
    public float DestH;
    public float Opacity;
    public uint Reserved;
}

[StructLayout(LayoutKind.Sequential)]
internal struct MixitInputStats
{
    public uint StructSize;
    public uint InputId;
    public ulong FramesProduced;
    public ulong FramesDropped;
    public ulong FramesPoolExhausted;
    public ulong FramesPresented;
    public ulong FramesRepeated;
    public ulong FramesSkipped;
    public ulong ProducerLateSkips;
    public ulong AudioBlocksProduced;
    public ulong AudioBlocksDropped;
    public ulong AudioBlocksConsumed;
    public ulong AvSyncEvents;
    public long AvOffsetLastNs;
    public long AvOffsetMaxAbsNs;
    public ulong DecodeErrors;   // API 0.3
    public ulong Loops;          // API 0.3
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct MixitFileSourceConfig
{
    public uint StructSize;
    public uint Flags;
    public byte* Path;
}

[StructLayout(LayoutKind.Sequential)]
internal struct MixitVideoFrameInfo
{
    public uint StructSize;
    public uint Width;
    public uint Height;
    public int PixelFormat;
    public uint InputId;
    public uint Flags;
    public long TickPtsNs;
    public long SourcePtsNs;
    public long SourceSequence;
    public ulong TickIndex;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct MixitMediaLibraryInfo
{
    public uint StructSize;
    public uint Flags;
    public uint AvcodecVersion;
    public uint AvformatVersion;
    public uint AvutilVersion;
    public uint Reserved;
    public fixed byte Version[64];
    public fixed byte License[64];
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct MixitCodecInfo
{
    public uint StructSize;
    public uint Flags;
    public fixed byte LongName[120];
}

/// <summary>Expected native sizes (bytes) of the ABI structs on x64.</summary>
internal static class NativeStructSizes
{
    public const int EngineConfig = 40;
    public const int LogRecord = 48;
    public const int EngineDiagnostics = 48;
    public const int SystemCapabilities = 232;
    public const int GpuAdapterInfo = 160;
    public const int SessionConfig = 32;
    public const int TestSourceConfig = 32;
    public const int SessionStats = 128;
    public const int LayerConfig = 48;
    public const int InputStats = 128;
    public const int FileSourceConfig = 16;
    public const int VideoFrameInfo = 56;
    public const int MediaLibraryInfo = 152;
    public const int CodecInfo = 128;
}
