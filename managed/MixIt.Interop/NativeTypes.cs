namespace MixIt.Interop;

/// <summary>Result codes returned by every native engine function (mirrors MIXIT_OK / MIXIT_E_*).</summary>
public enum NativeResult
{
    Ok = 0,
    InvalidArgument = -1,
    InvalidHandle = -2,
    InvalidState = -3,
    OutOfMemory = -4,
    NotSupported = -5,
    BufferTooSmall = -6,
    VersionMismatch = -7,
    NotFound = -8,
    Platform = -9,
    Io = -10,
    Internal = -99,
}

/// <summary>Native log levels (mirrors MIXIT_LOG_*).</summary>
public enum NativeLogLevel
{
    Trace = 0,
    Debug = 1,
    Information = 2,
    Warning = 3,
    Error = 4,
    Critical = 5,
    None = 6,
}

/// <summary>Engine lifecycle state (mirrors MIXIT_ENGINE_STATE_*).</summary>
public enum EngineState
{
    Initialized = 1,
    ShutDown = 2,
}

/// <summary>CPU instruction-set features usable by the engine (OS support already checked).</summary>
[Flags]
public enum CpuFeatures : uint
{
    None = 0,
    Sse2 = 1u << 0,
    Sse41 = 1u << 1,
    Sse42 = 1u << 2,
    Avx = 1u << 3,
    Avx2 = 1u << 4,
    Fma3 = 1u << 5,
    Avx512F = 1u << 6,
}

/// <summary>Which capability probes actually succeeded. A clear bit means "unknown", not "absent".</summary>
[Flags]
public enum DetectedComponents : uint
{
    None = 0,
    Cpu = 1u << 0,
    Memory = 1u << 1,
    OperatingSystem = 1u << 2,
    GpuAdapters = 1u << 3,
}

/// <summary>GPU adapter flags (mirrors MIXIT_GPU_ADAPTER_FLAG_*).</summary>
[Flags]
public enum GpuAdapterFlags : uint
{
    None = 0,
    Software = 1u << 0,
}

/// <summary>A log record produced by the native engine (or forwarded through it by the host).</summary>
public readonly record struct NativeLogEntry(
    NativeLogLevel Level,
    DateTimeOffset Timestamp,
    ulong ThreadId,
    ulong Sequence,
    string Category,
    string Message);

/// <summary>Engine health counters.</summary>
public readonly record struct EngineDiagnostics(
    EngineState State,
    TimeSpan Uptime,
    ulong LogRecordsAccepted,
    ulong LogRecordsDropped,
    ulong LogRecordsDelivered,
    ulong LogFileWriteErrors);

/// <summary>One GPU adapter as reported by the platform (DXGI on Windows).</summary>
public sealed record GpuAdapterInfo(
    uint VendorId,
    uint DeviceId,
    GpuAdapterFlags Flags,
    ulong DedicatedVideoMemoryBytes,
    ulong SharedSystemMemoryBytes,
    string Description)
{
    /// <summary>Human-readable vendor name derived from the PCI vendor id.</summary>
    public string VendorName => VendorId switch
    {
        0x10DE => "NVIDIA",
        0x1002 or 0x1022 => "AMD",
        0x8086 => "Intel",
        0x1414 => "Microsoft",
        0x5143 => "Qualcomm",
        _ => $"0x{VendorId:X4}",
    };

    /// <summary>True for software rasterizers such as the Microsoft Basic Render Driver.</summary>
    public bool IsSoftware => (Flags & GpuAdapterFlags.Software) != 0;
}

/// <summary>Hardware and OS capability snapshot.</summary>
public sealed record SystemCapabilities(
    DetectedComponents Detected,
    int LogicalProcessorCount,
    int PhysicalCoreCount,
    CpuFeatures CpuFeatures,
    ulong TotalPhysicalMemoryBytes,
    ulong AvailablePhysicalMemoryBytes,
    string CpuBrand,
    string OperatingSystem,
    IReadOnlyList<GpuAdapterInfo> GpuAdapters)
{
    /// <summary>True only if GPU enumeration actually ran; an empty list is then meaningful.</summary>
    public bool GpuEnumerationAvailable => (Detected & DetectedComponents.GpuAdapters) != 0;
}
