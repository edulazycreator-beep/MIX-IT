using System.Runtime.InteropServices;

namespace MixIt.Interop;

/// <summary>
/// Owns a native mixit_engine handle and the GC handle that keeps the managed
/// log dispatcher alive while native code may call back into it.
/// </summary>
/// <remarks>
/// Release order matters: mixit_engine_destroy joins the native logging
/// thread, so once it returns no callback can be running or start, and only
/// then is the dispatcher GC handle freed. If destroy fails, the GC handle is
/// intentionally leaked rather than freed under a possibly-live callback.
/// </remarks>
internal sealed class EngineHandle : SafeHandle
{
    private GCHandle _logDispatcher;

    public EngineHandle() : base(nint.Zero, ownsHandle: true)
    {
    }

    public override bool IsInvalid => handle == nint.Zero;

    internal void AttachLogDispatcher(GCHandle dispatcher) => _logDispatcher = dispatcher;

    protected override bool ReleaseHandle()
    {
        NativeResult result = NativeMethods.mixit_engine_destroy(handle);
        if (result != NativeResult.Ok)
        {
            return false;
        }
        if (_logDispatcher.IsAllocated)
        {
            _logDispatcher.Free();
        }
        return true;
    }
}

/// <summary>Owns a native mixit_session handle.</summary>
internal sealed class SessionHandle : SafeHandle
{
    public SessionHandle() : base(nint.Zero, ownsHandle: true)
    {
    }

    public override bool IsInvalid => handle == nint.Zero;

    protected override bool ReleaseHandle() => NativeMethods.mixit_session_destroy(handle) == NativeResult.Ok;
}
