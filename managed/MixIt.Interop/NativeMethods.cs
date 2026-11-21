using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace MixIt.Interop;

/// <summary>
/// Raw P/Invoke declarations for the native engine C ABI. Source-generated
/// (LibraryImport), so no runtime marshalling stubs are emitted and the
/// layer is trimming/AOT compatible. Every engine call takes the
/// <see cref="EngineHandle"/> SafeHandle, which ref-counts the handle for the
/// duration of the call so it cannot be released mid-call.
/// </summary>
internal static unsafe partial class NativeMethods
{
    /// <summary>
    /// Resolved as mixit_engine.dll on Windows and libmixit_engine.so on Linux
    /// (the runtime adds the platform prefix/suffix).
    /// </summary>
    public const string LibraryName = "mixit_engine";

    // Version of the C header this binding was written against. Must be kept
    // in sync with MIXIT_API_VERSION_* in mixit_engine.h.
    public const uint ExpectedApiMajor = 0;
    public const uint ExpectedApiMinor = 6;
    public const uint ExpectedApiPatch = 0;
    public const uint ExpectedApiVersion = (ExpectedApiMajor << 24) | (ExpectedApiMinor << 12) | ExpectedApiPatch;

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial uint mixit_get_api_version();

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial byte* mixit_get_build_info();

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial byte* mixit_result_name(NativeResult result);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial ulong mixit_get_monotonic_time_ns();

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_get_last_error_message(byte* buffer, uint capacity, uint* outRequired);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_engine_create(MixitEngineConfig* config, out EngineHandle engine);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_engine_shutdown(EngineHandle engine);

    /// <summary>Raw destroy, used only by <see cref="EngineHandle.ReleaseHandle"/>.</summary>
    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_engine_destroy(nint engine);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_engine_get_diagnostics(EngineHandle engine, MixitEngineDiagnostics* diagnostics);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_engine_set_log_level(EngineHandle engine, int level);

    [LibraryImport(LibraryName, StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_engine_log(EngineHandle engine, int level, string? category, string message);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_engine_detect_capabilities(EngineHandle engine, MixitSystemCapabilities* capabilities);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_engine_get_gpu_adapter(EngineHandle engine, uint index, MixitGpuAdapterInfo* adapter);

    // ---- sessions (API 0.2)

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_session_create(EngineHandle engine, MixitSessionConfig* config, out SessionHandle session);

    /// <summary>Raw destroy, used only by <see cref="SessionHandle.ReleaseHandle"/>.</summary>
    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_session_destroy(nint session);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_session_add_test_source(SessionHandle session, MixitTestSourceConfig* config, uint* inputId);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_session_add_file_source(SessionHandle session, MixitFileSourceConfig* config, uint* inputId);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_session_remove_input(SessionHandle session, uint inputId);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_session_set_program_input(SessionHandle session, uint inputId);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_session_start(SessionHandle session);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_session_stop(SessionHandle session);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_session_get_stats(SessionHandle session, MixitSessionStats* stats);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_session_get_input_stats(SessionHandle session, uint inputId, MixitInputStats* stats);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_session_copy_program_frame(SessionHandle session, byte* buffer, uint bufferSize,
        uint stride, MixitVideoFrameInfo* info);

    // ---- buses, transitions and layers (API 0.3)

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_session_set_preview_input(SessionHandle session, uint inputId);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_session_cut(SessionHandle session);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_session_start_transition(SessionHandle session, int kind, uint durationFrames);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_session_set_layer(SessionHandle session, int bus, uint index, MixitLayerConfig* layer);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_session_copy_bus_frame(SessionHandle session, int bus, byte* buffer, uint bufferSize,
        uint stride, MixitVideoFrameInfo* info);

    // ---- audio mixer (API 0.4)

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_session_set_audio_strip(SessionHandle session, uint inputId, MixitAudioStrip* strip);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_session_get_audio_strip(SessionHandle session, uint inputId, MixitAudioStrip* strip);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_session_set_audio_bus(SessionHandle session, uint bus, MixitAudioBusConfig* config);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_session_get_audio_bus(SessionHandle session, uint bus, MixitAudioBusConfig* config);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_session_set_monitor_source(SessionHandle session, uint bus);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_session_get_input_meter(SessionHandle session, uint inputId, MixitAudioMeter* meter);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_session_get_bus_meter(SessionHandle session, uint bus, MixitAudioMeter* meter);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_session_reset_loudness(SessionHandle session, uint bus);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_session_read_bus_audio(SessionHandle session, uint bus, float* dest,
        uint capacityFrames, MixitAudioBlockInfo* info);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_enumerate_audio_devices(EngineHandle engine, int direction,
        MixitAudioDeviceInfo* devices, uint capacity, uint* count);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_session_set_overlay(SessionHandle session, uint channel, MixitLayerConfig* layer);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_session_show_overlay(SessionHandle session, uint channel, int visible, uint fadeFrames);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_session_get_overlay(SessionHandle session, uint channel, MixitOverlayInfo* info);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_session_add_graphics_source(SessionHandle session, MixitGraphicsSourceConfig* config, uint* inputId);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_session_set_graphics_scene(SessionHandle session, uint inputId, byte* sceneJson);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_session_set_graphics_field(SessionHandle session, uint inputId, byte* name, byte* value);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_session_set_graphics_timer(SessionHandle session, uint inputId, int mode, long valueMs, int running);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_session_get_graphics_info(SessionHandle session, uint inputId, MixitGraphicsInfo* info);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_session_add_output(SessionHandle session, MixitOutputConfig* config, uint* id);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_session_remove_output(SessionHandle session, uint id);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_session_start_output(SessionHandle session, uint id);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_session_stop_output(SessionHandle session, uint id);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_session_get_output_stats(SessionHandle session, uint id, MixitOutputStats* stats);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_query_video_encoders(MixitVideoEncoderInfo* encoders, uint capacity, uint* count);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_scan_vst3(EngineHandle engine, byte* path, MixitVst3PluginInfo* plugins,
        uint capacity, uint* count);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_session_set_audio_insert(SessionHandle session, uint inputId,
        MixitAudioInsertConfig* config);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_session_remove_audio_insert(SessionHandle session, uint inputId, uint slot);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_session_set_audio_insert_bypass(SessionHandle session, uint inputId, uint slot,
        int bypass);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_session_get_audio_insert(SessionHandle session, uint inputId, uint slot,
        MixitAudioInsertInfo* info);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_session_get_audio_insert_parameter(SessionHandle session, uint inputId,
        uint slot, uint index, MixitAudioInsertParameter* parameter);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_session_set_audio_insert_parameter(SessionHandle session, uint inputId,
        uint slot, uint parameterId, double value);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_session_get_audio_insert_state(SessionHandle session, uint inputId, uint slot,
        byte* buffer, uint capacity, uint* size);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_session_set_audio_output(SessionHandle session, MixitAudioOutputConfig* config);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_session_get_audio_output_stats(SessionHandle session, MixitAudioOutputStats* stats);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_session_add_audio_capture_source(SessionHandle session,
        MixitAudioCaptureConfig* config, uint* inputId);

    // ---- capture devices (API 0.3, experimental)

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_enumerate_capture_devices(EngineHandle engine, MixitCaptureDeviceInfo* devices,
        uint capacity, uint* count);

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_session_add_capture_source(SessionHandle session, MixitCaptureSourceConfig* config,
        uint* inputId);

    // ---- media library (API 0.2)

    [LibraryImport(LibraryName)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_get_media_library_info(MixitMediaLibraryInfo* info);

    [LibraryImport(LibraryName, StringMarshalling = StringMarshalling.Utf8)]
    [UnmanagedCallConv(CallConvs = [typeof(CallConvCdecl)])]
    public static partial NativeResult mixit_query_codec(string name, MixitCodecInfo* info);

    /// <summary>Reads the calling thread's last native error message.</summary>
    public static string GetLastErrorMessage()
    {
        Span<byte> buffer = stackalloc byte[1024];
        fixed (byte* p = buffer)
        {
            _ = mixit_get_last_error_message(p, (uint)buffer.Length, null);
            return Utf8.FromNullTerminated(p, buffer.Length);
        }
    }
}

/// <summary>UTF-8 helpers for native strings.</summary>
internal static unsafe class Utf8
{
    public static string FromNullTerminated(byte* text, int maxLength)
    {
        if (text is null) return string.Empty;
        var span = new ReadOnlySpan<byte>(text, maxLength);
        int nul = span.IndexOf((byte)0);
        return System.Text.Encoding.UTF8.GetString(nul >= 0 ? span[..nul] : span);
    }

    public static string FromNullTerminated(byte* text) =>
        text is null ? string.Empty : Marshal.PtrToStringUTF8((nint)text) ?? string.Empty;
}
