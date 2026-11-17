namespace MixIt.Interop;

/// <summary>A native engine call failed. Carries the native result code and message.</summary>
public sealed class MixItNativeException : Exception
{
    public MixItNativeException(NativeResult result, string function, string nativeMessage)
        : base(Format(result, function, nativeMessage))
    {
        Result = result;
        Function = function;
        NativeMessage = nativeMessage;
    }

    public MixItNativeException(string message, Exception innerException)
        : base(message, innerException)
    {
        Result = NativeResult.Internal;
        Function = string.Empty;
        NativeMessage = string.Empty;
    }

    /// <summary>The native result code.</summary>
    public NativeResult Result { get; }

    /// <summary>Native function that failed, e.g. "mixit_engine_create".</summary>
    public string Function { get; }

    /// <summary>The calling thread's native last-error message at the time of failure.</summary>
    public string NativeMessage { get; }

    private static string Format(NativeResult result, string function, string nativeMessage) =>
        string.IsNullOrEmpty(nativeMessage)
            ? $"{function} failed with {result}"
            : $"{function} failed with {result}: {nativeMessage}";
}
