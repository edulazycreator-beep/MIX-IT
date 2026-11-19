using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace MixIt.Interop;

/// <summary>Options for <see cref="NativeEngine.Create"/>.</summary>
public sealed class NativeEngineOptions
{
    /// <summary>Records below this level are discarded inside the engine.</summary>
    public NativeLogLevel MinimumLogLevel { get; init; } = NativeLogLevel.Information;

    /// <summary>Bounded log queue size in records; 0 selects the engine default (4096).</summary>
    public uint LogQueueCapacity { get; init; }

    /// <summary>Optional path of the JSON-lines log file written by the engine.</summary>
    public string? LogFilePath { get; init; }

    /// <summary>
    /// Optional handler for every delivered log record. Invoked on the engine's
    /// logging thread, never on a media thread. Must be fast and must not
    /// dispose the engine. Exceptions are caught and counted.
    /// </summary>
    public Action<NativeLogEntry>? LogHandler { get; init; }
}

/// <summary>
/// Managed owner of one native engine instance. Thread-safe. Dispose (or
/// <see cref="Shutdown"/>) stops the engine; after Dispose returns the log
/// handler is never invoked again.
/// </summary>
public sealed unsafe class NativeEngine : IDisposable
{
    private readonly EngineHandle _handle;
    private readonly LogDispatcher _dispatcher;

    private NativeEngine(EngineHandle handle, LogDispatcher dispatcher)
    {
        _handle = handle;
        _dispatcher = dispatcher;
    }

    /// <summary>API version this binding was compiled against.</summary>
    public static Version BindingApiVersion { get; } =
        new((int)NativeMethods.ExpectedApiMajor, (int)NativeMethods.ExpectedApiMinor, (int)NativeMethods.ExpectedApiPatch);

    /// <summary>API version of the loaded native library.</summary>
    public static Version LibraryApiVersion
    {
        get
        {
            uint v = Call(static () => NativeMethods.mixit_get_api_version());
            return new Version((int)((v >> 24) & 0xFF), (int)((v >> 12) & 0xFFF), (int)(v & 0xFFF));
        }
    }

    /// <summary>Build description of the loaded native library.</summary>
    public static string BuildInfo => Call(static () => Utf8.FromNullTerminated(NativeMethods.mixit_get_build_info()));

    /// <summary>The engine master clock (monotonic nanoseconds).</summary>
    public static ulong MonotonicTimeNanoseconds => NativeMethods.mixit_get_monotonic_time_ns();

    /// <summary>Number of exceptions thrown by the log handler (caught at the native boundary).</summary>
    public long LogHandlerFailures => Interlocked.Read(ref _dispatcher.Failures);

    /// <summary>
    /// Verifies that the loaded native library implements a compatible ABI.
    /// While the API major version is 0 the minor version must match exactly.
    /// </summary>
    public static void EnsureCompatibleLibrary()
    {
        Version lib = LibraryApiVersion;
        Version bind = BindingApiVersion;
        bool compatible = lib.Major == bind.Major && (bind.Major == 0 ? lib.Minor == bind.Minor : lib.Minor >= bind.Minor);
        if (!compatible)
        {
            throw new MixItNativeException(NativeResult.VersionMismatch, nameof(EnsureCompatibleLibrary),
                $"native engine API {lib} is incompatible with managed binding API {bind}");
        }
    }

    /// <summary>Lifecycle step 1: creates and initializes an engine.</summary>
    /// <exception cref="MixItNativeException">The engine could not be created.</exception>
    public static NativeEngine Create(NativeEngineOptions options)
    {
        ArgumentNullException.ThrowIfNull(options);
        EnsureCompatibleLibrary();

        var dispatcher = new LogDispatcher(options.LogHandler);
        GCHandle dispatcherHandle = GCHandle.Alloc(dispatcher);
        bool handedToEngine = false;
        byte* path = null;
        try
        {
            path = (byte*)Marshal.StringToCoTaskMemUTF8(options.LogFilePath);
            var config = new MixitEngineConfig
            {
                StructSize = (uint)sizeof(MixitEngineConfig),
                ApiVersion = NativeMethods.ExpectedApiVersion,
                MinLogLevel = (int)options.MinimumLogLevel,
                LogQueueCapacity = options.LogQueueCapacity,
                LogFilePath = path,
                LogCallback = options.LogHandler is null ? null : &OnNativeLog,
                LogUserData = (void*)GCHandle.ToIntPtr(dispatcherHandle),
            };

            NativeResult result = NativeMethods.mixit_engine_create(&config, out EngineHandle handle);
            if (result != NativeResult.Ok)
            {
                string message = NativeMethods.GetLastErrorMessage();
                handle.Dispose();
                throw new MixItNativeException(result, "mixit_engine_create", message);
            }
            handle.AttachLogDispatcher(dispatcherHandle);  // freed by the handle after native destroy
            handedToEngine = true;
            return new NativeEngine(handle, dispatcher);
        }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException)
        {
            throw NotLoaded(ex);
        }
        finally
        {
            Marshal.FreeCoTaskMem((nint)path);
            if (!handedToEngine)
            {
                dispatcherHandle.Free();  // no engine exists, so no callback can be pending
            }
        }
    }

    /// <summary>Lifecycle step 2: probes hardware and OS capabilities.</summary>
    public SystemCapabilities DetectCapabilities()
    {
        MixitSystemCapabilities caps = default;
        caps.StructSize = (uint)sizeof(MixitSystemCapabilities);
        Check(NativeMethods.mixit_engine_detect_capabilities(_handle, &caps), "mixit_engine_detect_capabilities");

        var adapters = new GpuAdapterInfo[caps.GpuAdapterCount];
        for (uint i = 0; i < caps.GpuAdapterCount; i++)
        {
            MixitGpuAdapterInfo a = default;
            a.StructSize = (uint)sizeof(MixitGpuAdapterInfo);
            Check(NativeMethods.mixit_engine_get_gpu_adapter(_handle, i, &a), "mixit_engine_get_gpu_adapter");
            adapters[i] = new GpuAdapterInfo(a.VendorId, a.DeviceId, (GpuAdapterFlags)a.Flags,
                a.DedicatedVideoMemoryBytes, a.SharedSystemMemoryBytes, Utf8.FromNullTerminated(a.Description, 128));
        }

        return new SystemCapabilities(
            (DetectedComponents)caps.DetectionFlags,
            (int)caps.LogicalProcessorCount,
            (int)caps.PhysicalCoreCount,
            (CpuFeatures)caps.CpuFeatures,
            caps.TotalPhysicalMemoryBytes,
            caps.AvailablePhysicalMemoryBytes,
            Utf8.FromNullTerminated(caps.CpuBrand, 64),
            Utf8.FromNullTerminated(caps.OsDescription, 128),
            adapters);
    }

    /// <summary>Lifecycle step 3: creates a production session owned by this engine.</summary>
    public NativeSession CreateSession(SessionConfiguration configuration)
    {
        ArgumentNullException.ThrowIfNull(configuration);
        var c = new MixitSessionConfig
        {
            StructSize = (uint)sizeof(MixitSessionConfig),
            Width = configuration.Width,
            Height = configuration.Height,
            FrameRateNum = configuration.FrameRate.Numerator,
            FrameRateDen = configuration.FrameRate.Denominator,
            AudioSampleRate = configuration.AudioSampleRate,
            AudioChannels = configuration.AudioChannels,
            ProgramPoolFrames = configuration.ProgramPoolFrames,
            Compositor = (int)configuration.Compositor,
        };
        NativeResult r = NativeMethods.mixit_session_create(_handle, &c, out SessionHandle session);
        if (r != NativeResult.Ok)
        {
            string message = NativeMethods.GetLastErrorMessage();
            session.Dispose();
            throw new MixItNativeException(r, "mixit_session_create", message);
        }
        return new NativeSession(session);
    }

    /// <summary>
    /// Connected video capture devices (EXPERIMENTAL; Windows Media Foundation).
    /// Empty when there are none; throws with <see cref="NativeResult.NotSupported"/>
    /// where no capture backend exists.
    /// </summary>
    public IReadOnlyList<CaptureDevice> EnumerateCaptureDevices()
    {
        uint count = 0;
        Check(NativeMethods.mixit_enumerate_capture_devices(_handle, null, 0, &count), "mixit_enumerate_capture_devices");
        if (count == 0) return [];
        var raw = new MixitCaptureDeviceInfo[count + 4];  // devices may appear between the two calls
        for (int i = 0; i < raw.Length; i++) raw[i].StructSize = (uint)sizeof(MixitCaptureDeviceInfo);
        fixed (MixitCaptureDeviceInfo* p = raw)
        {
            Check(NativeMethods.mixit_enumerate_capture_devices(_handle, p, (uint)raw.Length, &count), "mixit_enumerate_capture_devices");
        }
        var list = new List<CaptureDevice>();
        for (int i = 0; i < Math.Min(count, (uint)raw.Length); i++)
        {
            fixed (MixitCaptureDeviceInfo* d = &raw[i])
            {
                string id = Utf8.FromNullTerminated(d->Id, 512);
                if (id.Length > 0) list.Add(new CaptureDevice(id, Utf8.FromNullTerminated(d->Name, 128)));
            }
        }
        return list;
    }

    /// <summary>
    /// Active audio devices of one direction (EXPERIMENTAL; WASAPI). Throws
    /// with <see cref="NativeResult.NotSupported"/> where no backend exists.
    /// </summary>
    public IReadOnlyList<AudioDevice> EnumerateAudioDevices(AudioDeviceDirection direction)
    {
        uint count = 0;
        Check(NativeMethods.mixit_enumerate_audio_devices(_handle, (int)direction, null, 0, &count), "mixit_enumerate_audio_devices");
        if (count == 0) return [];
        var raw = new MixitAudioDeviceInfo[count + 4];
        for (int i = 0; i < raw.Length; i++) raw[i].StructSize = (uint)sizeof(MixitAudioDeviceInfo);
        fixed (MixitAudioDeviceInfo* p = raw)
        {
            Check(NativeMethods.mixit_enumerate_audio_devices(_handle, (int)direction, p, (uint)raw.Length, &count),
                "mixit_enumerate_audio_devices");
        }
        var list = new List<AudioDevice>();
        for (int i = 0; i < Math.Min(count, (uint)raw.Length); i++)
        {
            MixitAudioDeviceInfo d = raw[i];
            string id = Utf8.FromNullTerminated(d.Id, 256);
            if (id.Length == 0) continue;
            list.Add(new AudioDevice(id, Utf8.FromNullTerminated(d.Name, 128), direction, (d.Flags & 1) != 0,
                (int)d.Channels, (int)d.SampleRate));
        }
        return list;
    }

    /// <summary>FFmpeg availability, versions and licence verdict for this engine build.</summary>
    public static MediaLibraryInfo GetMediaLibraryInfo()
    {
        MixitMediaLibraryInfo i = default;
        i.StructSize = (uint)sizeof(MixitMediaLibraryInfo);
        NativeResult r;
        try
        {
            r = NativeMethods.mixit_get_media_library_info(&i);
        }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException)
        {
            throw NotLoaded(ex);
        }
        Check(r, "mixit_get_media_library_info");
        uint v = i.AvcodecVersion;
        return new MediaLibraryInfo(
            (i.Flags & 1u) != 0, (i.Flags & 2u) != 0, (i.Flags & 4u) != 0, (i.Flags & 8u) != 0, (i.Flags & 16u) != 0,
            Utf8.FromNullTerminated(i.Version, 64), Utf8.FromNullTerminated(i.License, 64),
            v == 0 ? null : new Version((int)(v >> 16), (int)((v >> 8) & 0xFF), (int)(v & 0xFF)));
    }

    /// <summary>
    /// Looks up an FFmpeg codec by name ("h264", "h264_nvenc", ...). Returns null
    /// if it is not in the build or FFmpeg is unusable. Presence does not prove
    /// the hardware can run it.
    /// </summary>
    public static CodecInfo? QueryCodec(string name)
    {
        ArgumentException.ThrowIfNullOrEmpty(name);
        MixitCodecInfo c = default;
        c.StructSize = (uint)sizeof(MixitCodecInfo);
        NativeResult r = NativeMethods.mixit_query_codec(name, &c);
        if (r is NativeResult.NotFound or NativeResult.NotSupported) return null;
        Check(r, "mixit_query_codec");
        return new CodecInfo(name, (c.Flags & 1u) != 0, (c.Flags & 2u) != 0, (c.Flags & 4u) != 0,
            Utf8.FromNullTerminated(c.LongName, 120));
    }

    /// <summary>Reads engine health counters. Valid after <see cref="Shutdown"/>, not after Dispose.</summary>
    public EngineDiagnostics GetDiagnostics()
    {
        MixitEngineDiagnostics d = default;
        d.StructSize = (uint)sizeof(MixitEngineDiagnostics);
        Check(NativeMethods.mixit_engine_get_diagnostics(_handle, &d), "mixit_engine_get_diagnostics");
        return new EngineDiagnostics((EngineState)d.State, TimeSpan.FromTicks((long)(d.UptimeNs / 100)),
            d.LogRecordsAccepted, d.LogRecordsDropped, d.LogRecordsDelivered, d.LogFileWriteErrors);
    }

    /// <summary>Changes the engine's minimum log level at runtime.</summary>
    public void SetLogLevel(NativeLogLevel level) =>
        Check(NativeMethods.mixit_engine_set_log_level(_handle, (int)level), "mixit_engine_set_log_level");

    /// <summary>
    /// Writes a host record into the engine's ordered log stream. Never blocks;
    /// returns false if the engine is no longer accepting records (shut down or disposed).
    /// </summary>
    public bool TryLog(NativeLogLevel level, string category, string message)
    {
        ArgumentNullException.ThrowIfNull(message);
        if (level is < NativeLogLevel.Trace or >= NativeLogLevel.None) return false;
        bool added = false;
        try
        {
            _handle.DangerousAddRef(ref added);
        }
        catch (ObjectDisposedException)
        {
            return false;
        }
        try
        {
            return NativeMethods.mixit_engine_log(_handle, (int)level, category, message) == NativeResult.Ok;
        }
        finally
        {
            if (added) _handle.DangerousRelease();
        }
    }

    /// <summary>Lifecycle step 9: stops the engine and flushes its log. Idempotent.</summary>
    public void Shutdown()
    {
        ThrowIfOnLogThread(nameof(Shutdown));
        if (_handle.IsClosed) return;
        Check(NativeMethods.mixit_engine_shutdown(_handle), "mixit_engine_shutdown");
    }

    /// <summary>Shuts down and releases the native engine.</summary>
    public void Dispose()
    {
        ThrowIfOnLogThread(nameof(Dispose));
        _handle.Dispose();
    }

    private static void Check(NativeResult result, string function)
    {
        if (result != NativeResult.Ok)
        {
            throw new MixItNativeException(result, function, NativeMethods.GetLastErrorMessage());
        }
    }

    private static void ThrowIfOnLogThread(string operation)
    {
        if (LogDispatcher.IsOnLogThread)
        {
            throw new InvalidOperationException(
                $"{operation} must not be called from a native log handler (the engine would have to join its own logging thread).");
        }
    }

    private static T Call<T>(Func<T> nativeCall)
    {
        try
        {
            return nativeCall();
        }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException)
        {
            throw NotLoaded(ex);
        }
    }

    private static MixItNativeException NotLoaded(Exception ex) =>
        new($"The MIX IT native engine ({NativeMethods.LibraryName}) could not be loaded. Build the native " +
            "engine and place it next to the application (see docs/BUILD_GUIDE.md).", ex);

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void OnNativeLog(MixitLogRecord* record, void* userData)
    {
        // Runs on the native "mixit-log" thread. Nothing may propagate out.
        LogDispatcher? dispatcher = null;
        try
        {
            LogDispatcher.IsOnLogThread = true;
            dispatcher = GCHandle.FromIntPtr((nint)userData).Target as LogDispatcher;
            dispatcher?.Dispatch(record);
        }
        catch
        {
            if (dispatcher is not null) Interlocked.Increment(ref dispatcher.Failures);
        }
    }

    private sealed class LogDispatcher(Action<NativeLogEntry>? handler)
    {
        [ThreadStatic]
        public static bool IsOnLogThread;

        public long Failures;

        public void Dispatch(MixitLogRecord* r)
        {
            if (handler is null) return;
            var entry = new NativeLogEntry(
                (NativeLogLevel)r->Level,
                new DateTimeOffset(DateTime.UnixEpoch.Ticks + (r->UnixTimeNs / 100), TimeSpan.Zero),
                r->ThreadId,
                r->Sequence,
                Utf8.FromNullTerminated(r->Category),
                Utf8.FromNullTerminated(r->Message));
            handler(entry);
        }
    }
}
