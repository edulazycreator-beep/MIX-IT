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
    public int Compositor;   // API 0.3: MIXIT_COMPOSITOR_PREFER_*
    public uint Reserved;    // API 0.3
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
    public ulong CompositorFallbacks;    // API 0.3
    public ulong AudioBlocks;            // API 0.4
    public ulong AudioLateBlocks;        // API 0.4
    public ulong AudioSkippedBlocks;     // API 0.4
    public long AudioMaxLatenessNs;      // API 0.4
}

[StructLayout(LayoutKind.Sequential)]
internal struct MixitEqBand
{
    public int Enabled;
    public int Type;
    public float FrequencyHz;
    public float GainDb;
    public float Q;
}

[StructLayout(LayoutKind.Sequential)]
internal struct MixitAudioStrip
{
    public uint StructSize;
    public uint Flags;
    public float TrimDb;
    public float FaderDb;
    public float Balance;
    public float DelayMs;
    public uint BusMask;
    public float HighPassHz;
    public MixitEqBand Eq0;
    public MixitEqBand Eq1;
    public MixitEqBand Eq2;
    public MixitEqBand Eq3;
    public float GateThresholdDb;
    public float GateRatio;
    public float GateRangeDb;
    public float GateAttackMs;
    public float GateHoldMs;
    public float GateReleaseMs;
    public float CompThresholdDb;
    public float CompRatio;
    public float CompKneeDb;
    public float CompAttackMs;
    public float CompReleaseMs;
    public float CompMakeupDb;
}

[StructLayout(LayoutKind.Sequential)]
internal struct MixitAudioBusConfig
{
    public uint StructSize;
    public uint Flags;
    public float FaderDb;
    public float LimiterCeilingDb;
    public float LimiterReleaseMs;
    public uint Reserved;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct MixitAudioMeter
{
    public uint StructSize;
    public uint Id;
    public uint Channels;
    public uint Flags;
    public fixed float PeakDb[8];
    public fixed float RmsDb[8];
    public float GainReductionDb;
    public float GateReductionDb;
    public float AppliedGainDb;
    public float MomentaryLufs;
    public float ShortTermLufs;
    public float IntegratedLufs;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct MixitAudioDeviceInfo
{
    public uint StructSize;
    public int Direction;
    public uint Flags;
    public uint Channels;
    public uint SampleRate;
    public uint Reserved;
    public fixed byte Name[128];
    public fixed byte Id[256];
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct MixitOutputConfig
{
    public uint StructSize;
    public uint SourceInput;
    public byte* Url;
    public byte* Format;
    public byte* VideoEncoder;
    public uint VideoKbps;
    public uint KeyframeMs;
    public uint AudioKbps;
    public uint Flags;
    public uint SegmentSeconds;
    public uint Reserved;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct MixitOutputStats
{
    public uint StructSize;
    public int State;
    public uint Flags;
    public uint Segments;
    public ulong BytesWritten;
    public ulong VideoPackets;
    public ulong AudioPackets;
    public ulong DroppedPackets;
    public ulong EncoderDroppedFrames;
    public ulong Reconnects;
    public uint BitrateKbps;
    public uint QueueMs;
    public long DurationNs;
    public long FreeBytes;
    public fixed byte Encoder[32];
    public fixed byte CurrentFile[512];
    public fixed byte LastError[256];
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct MixitVideoEncoderInfo
{
    public uint StructSize;
    public uint Flags;
    public fixed byte Name[32];
    public fixed byte LongName[128];
    public fixed byte Error[192];
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct MixitVst3PluginInfo
{
    public uint StructSize;
    public uint Reserved;
    public fixed byte ClassId[40];
    public fixed byte Name[128];
    public fixed byte Vendor[128];
    public fixed byte Version[64];
    public fixed byte Subcategories[128];
    public fixed byte Path[512];
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct MixitAudioInsertConfig
{
    public uint StructSize;
    public uint Slot;
    public byte* Path;
    public byte* ClassId;
    public byte* State;
    public uint StateSize;
    public uint Reserved;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct MixitAudioInsertInfo
{
    public uint StructSize;
    public uint Flags;
    public uint ParameterCount;
    public uint LatencySamples;
    public ulong ProcessErrors;
    public fixed byte ClassId[40];
    public fixed byte Name[128];
    public fixed byte Vendor[128];
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct MixitAudioInsertParameter
{
    public uint StructSize;
    public uint Id;
    public uint Flags;
    public int StepCount;
    public double DefaultValue;
    public double Value;
    public fixed byte Title[128];
    public fixed byte Units[32];
    public fixed byte Display[64];
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct MixitAudioOutputConfig
{
    public uint StructSize;
    public uint Bus;
    public byte* DeviceId;
    public uint LatencyMs;
    public uint Reserved;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct MixitAudioOutputStats
{
    public uint StructSize;
    public int State;
    public uint Bus;
    public uint BufferedMs;
    public ulong Underruns;
    public ulong DeviceErrors;
    public ulong DroppedFrames;
    public double DriftRatio;
    public fixed byte DeviceName[128];
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct MixitAudioCaptureConfig
{
    public uint StructSize;
    public uint Flags;
    public byte* DeviceId;
}

[StructLayout(LayoutKind.Sequential)]
internal struct MixitAudioBlockInfo
{
    public uint StructSize;
    public uint Frames;
    public long PtsNs;
    public long FirstSample;
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
    public int KeyType;
    public uint KeyColor;
    public float KeySimilarity;
    public float KeySmoothness;
    public float KeySpill;
    public float LumaThreshold;
    public float LumaSoftness;
    public uint KeyFlags;
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
    public ulong AudioUnderrunFrames;  // API 0.4
    public ulong AudioLateBlocks;      // API 0.4
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct MixitFileSourceConfig
{
    public uint StructSize;
    public uint Flags;
    public byte* Path;
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct MixitCaptureDeviceInfo
{
    public uint StructSize;
    public uint Flags;
    public fixed byte Name[128];
    public fixed byte Id[512];
}

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct MixitCaptureSourceConfig
{
    public uint StructSize;
    public uint Flags;
    public byte* DeviceId;
    public uint Width;
    public uint Height;
    public uint FrameRateNum;
    public uint FrameRateDen;
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
    public const int SessionConfig = 40;
    public const int TestSourceConfig = 32;
    public const int SessionStats = 168;
    public const int AudioStrip = 160;
    public const int AudioBusConfig = 24;
    public const int AudioMeter = 104;
    public const int AudioBlockInfo = 24;
    public const int AudioDeviceInfo = 408;
    public const int AudioOutputConfig = 24;
    public const int AudioOutputStats = 176;
    public const int AudioCaptureConfig = 16;
    public const int Vst3PluginInfo = 1008;
    public const int OutputConfig = 56;
    public const int OutputStats = 888;
    public const int VideoEncoderInfo = 360;
    public const int AudioInsertConfig = 40;
    public const int AudioInsertInfo = 320;
    public const int AudioInsertParameter = 256;
    public const int LayerConfig = 80;
    public const int CaptureDeviceInfo = 648;
    public const int CaptureSourceConfig = 32;
    public const int InputStats = 144;
    public const int FileSourceConfig = 16;
    public const int VideoFrameInfo = 56;
    public const int MediaLibraryInfo = 152;
    public const int CodecInfo = 128;
}
