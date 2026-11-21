using System.Text.Json;
using System.Text.Json.Serialization;

namespace MixIt.Protocol;

/// <summary>Versions and well-known paths of the remote-control protocol (docs/REMOTE_API.md).</summary>
public static class ProtocolInfo
{
    /// <summary>Protocol version; a client should check the major number.</summary>
    public const string Version = "1.0";

    /// <summary>Base path of every HTTP endpoint.</summary>
    public const string BasePath = "/api/v1";

    /// <summary>Default TCP port of the remote-control server.</summary>
    public const int DefaultPort = 8099;

    /// <summary>JSON options of every protocol message (camelCase, enums as strings).</summary>
    public static JsonSerializerOptions Json { get; } = new(JsonSerializerDefaults.Web)
    {
        Converters = { new JsonStringEnumConverter(JsonNamingPolicy.CamelCase) },
        WriteIndented = false,
    };
}

/// <summary>Type of a command parameter (all arguments travel as strings).</summary>
public enum ParameterKind
{
    String,
    Integer,
    Number,
    Boolean,
    /// <summary>An input: its number (1 = first), <c>id:N</c> (engine id) or its name.</summary>
    Input,
}

/// <summary>One parameter of a command.</summary>
public sealed record CommandParameter(string Name, ParameterKind Kind, bool Required, string Description);

/// <summary>A command the production accepts (from the UI, macros, automation, the remote API or extensions).</summary>
public sealed record CommandDescriptor(string Name, string Description, IReadOnlyList<CommandParameter> Parameters, string Category = "Production");

/// <summary>A command call. Argument names are case-insensitive.</summary>
public sealed record CommandRequest(string Name, IReadOnlyDictionary<string, string> Args)
{
    public static CommandRequest Of(string name, params (string Key, string Value)[] args) =>
        new(name, args.ToDictionary(a => a.Key, a => a.Value, StringComparer.OrdinalIgnoreCase));
}

/// <summary>Outcome of a command. <see cref="Value"/> carries an optional result (e.g. a mark time).</summary>
public sealed record CommandResult(bool Ok, string? Error = null, string? Value = null)
{
    public static CommandResult Success(string? value = null) => new(true, null, value);
    public static CommandResult Failure(string error) => new(false, error);
}

/// <summary>Tally of one input. Program wins over preview.</summary>
public sealed record TallyEntry(uint InputId, int Number, string Name, bool Program, bool Preview);

/// <summary>An input as remote clients see it.</summary>
public sealed record InputInfo(uint Id, int Number, string Name, string Kind);

/// <summary>A recording/stream destination as remote clients see it (addresses redacted).</summary>
public sealed record OutputInfo(int Id, string Name, string Kind, string State, string Address);

/// <summary>Snapshot of the production for remote clients.</summary>
public sealed record ProductionState(
    bool EngineAvailable,
    bool Running,
    string Format,
    uint ProgramInputId,
    uint PreviewInputId,
    IReadOnlyList<InputInfo> Inputs,
    IReadOnlyList<OutputInfo> Outputs,
    IReadOnlyList<TallyEntry> Tally);

/// <summary>Event pushed on the event stream: <c>tally</c>, <c>state</c>, <c>command</c> or <c>macro</c>.</summary>
public sealed record ServerEvent(string Type, DateTimeOffset Time, JsonElement Data);
