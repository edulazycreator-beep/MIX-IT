using MixIt.Protocol;

namespace MixIt.SDK;

/// <summary>
/// Entry point of an extension (EXPERIMENTAL, SDK version <see cref="SdkInfo.Version"/>).
/// The host creates one instance of every public non-abstract type implementing
/// this interface with a parameterless constructor, calls <see cref="Initialize"/>
/// once on the production thread, and <see cref="IDisposable.Dispose"/> at shutdown.
/// Extensions run inside the MIX IT process with the operator's rights: only
/// install extensions you trust.
/// </summary>
public interface IMixItExtension : IDisposable
{
    /// <summary>Stable identifier, e.g. "com.example.stream-deck".</summary>
    string Id { get; }

    /// <summary>Name shown to the operator.</summary>
    string DisplayName { get; }

    void Initialize(IExtensionHost host);
}

/// <summary>What the host offers an extension.</summary>
public interface IExtensionHost
{
    /// <summary>SDK version implemented by the host.</summary>
    string SdkVersion { get; }

    /// <summary>Every command currently registered (built-in and from extensions).</summary>
    IReadOnlyList<CommandDescriptor> Commands { get; }

    /// <summary>Runs a command on the production thread. Never throws for command errors: see the result.</summary>
    Task<CommandResult> ExecuteAsync(CommandRequest request, CancellationToken cancellationToken = default);

    /// <summary>
    /// Adds a command (name must be unique; extensions should prefix it, e.g. "Example.DoThing").
    /// The handler runs on the production thread.
    /// </summary>
    void RegisterCommand(CommandDescriptor descriptor, Func<CommandRequest, Task<CommandResult>> handler);

    /// <summary>Raised (on the production thread) when any input's tally changes.</summary>
    event EventHandler<IReadOnlyList<TallyEntry>>? TallyChanged;

    /// <summary>Raised after every command (from any source).</summary>
    event EventHandler<(CommandRequest Request, CommandResult Result)>? CommandExecuted;

    /// <summary>Writes to the MIX IT log.</summary>
    void Log(string message, bool isError = false);
}

/// <summary>SDK constants.</summary>
public static class SdkInfo
{
    public const string Version = "0.1";
}
