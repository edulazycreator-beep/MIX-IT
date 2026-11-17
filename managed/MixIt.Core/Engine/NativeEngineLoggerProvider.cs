using Microsoft.Extensions.Logging;
using MixIt.Interop;

namespace MixIt.Core.Engine;

/// <summary>
/// ILoggerProvider that writes managed log records into the native engine's
/// log, so the engine's JSON-lines file holds one ordered stream for the
/// whole application. While the engine is not running, records are not
/// forwarded (other providers, e.g. Debug, still receive them).
/// </summary>
/// <remarks>
/// Native records are never fed back into ILogger, so this cannot loop.
/// </remarks>
public sealed class NativeEngineLoggerProvider(EngineHost host) : ILoggerProvider
{
    private readonly EngineHost _host = host ?? throw new ArgumentNullException(nameof(host));

    /// <inheritdoc />
    public ILogger CreateLogger(string categoryName) => new ForwardingLogger(_host, categoryName);

    /// <inheritdoc />
    public void Dispose()
    {
    }

    internal static NativeLogLevel Map(LogLevel level) => level switch
    {
        LogLevel.Trace => NativeLogLevel.Trace,
        LogLevel.Debug => NativeLogLevel.Debug,
        LogLevel.Information => NativeLogLevel.Information,
        LogLevel.Warning => NativeLogLevel.Warning,
        LogLevel.Error => NativeLogLevel.Error,
        LogLevel.Critical => NativeLogLevel.Critical,
        _ => NativeLogLevel.None,
    };

    private sealed class ForwardingLogger(EngineHost host, string category) : ILogger
    {
        public IDisposable? BeginScope<TState>(TState state) where TState : notnull => null;

        public bool IsEnabled(LogLevel logLevel) => logLevel != LogLevel.None && host.IsRunning;

        public void Log<TState>(LogLevel logLevel, EventId eventId, TState state, Exception? exception,
            Func<TState, Exception?, string> formatter)
        {
            if (!IsEnabled(logLevel)) return;
            string message = formatter(state, exception);
            if (exception is not null)
            {
                message = $"{message} | {exception.GetType().Name}: {exception.Message}";
            }
            host.TryForwardLog(Map(logLevel), category, message);
        }
    }
}
