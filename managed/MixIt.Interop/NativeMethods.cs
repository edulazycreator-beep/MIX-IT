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
    public const uint ExpectedApiMinor = 3;
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
