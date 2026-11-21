using System.Globalization;
using Microsoft.Extensions.Logging;
using MixIt.Protocol;

namespace MixIt.Core.Control;

/// <summary>
/// Runs work on the thread that owns the production (the UI thread in the
/// desktop app). <see cref="Production.ProductionService"/> is single-threaded
/// by contract, so every command goes through this.
/// </summary>
public interface IProductionDispatcher
{
    Task<T> InvokeAsync<T>(Func<Task<T>> work);
}

/// <summary>
/// Serializes work without a UI thread (tests, headless hosts): one item at a time, in order.
/// Work started from inside a dispatched item (a macro run by a command) runs
/// inline, like a UI dispatcher called from its own thread.
/// </summary>
public sealed class SerialDispatcher : IProductionDispatcher, IDisposable
{
    private readonly SemaphoreSlim _gate = new(1, 1);
    private readonly AsyncLocal<bool> _inside = new();

    public async Task<T> InvokeAsync<T>(Func<Task<T>> work)
    {
        ArgumentNullException.ThrowIfNull(work);
        if (_inside.Value) return await work().ConfigureAwait(false);
        await _gate.WaitAsync().ConfigureAwait(false);
        try
        {
            _inside.Value = true;
            return await work().ConfigureAwait(false);
        }
        finally
        {
            _inside.Value = false;
            _gate.Release();
        }
    }

    public void Dispose() => _gate.Dispose();
}

/// <summary>Thrown by a command handler for an operator-facing error (bad argument, nothing to do).</summary>
public sealed class CommandException(string message) : Exception(message);

/// <summary>Typed access to a command's string arguments, with clear errors.</summary>
public sealed class CommandArgs
{
    private readonly Dictionary<string, string> _args;

    public CommandArgs(IReadOnlyDictionary<string, string> args)
    {
        _args = new Dictionary<string, string>(args, StringComparer.OrdinalIgnoreCase);
    }

    /// <summary>The arguments as given.</summary>
    public IReadOnlyDictionary<string, string> Raw => _args;

    public bool Has(string name) => _args.TryGetValue(name, out string? v) && v.Length > 0;

    public string String(string name, string? fallback = null) =>
        _args.TryGetValue(name, out string? v) && v.Length > 0 ? v
        : fallback ?? throw new CommandException($"Missing argument '{name}'.");

    public int Int(string name, int? fallback = null, int min = int.MinValue, int max = int.MaxValue)
    {
        if (!Has(name)) return fallback ?? throw new CommandException($"Missing argument '{name}'.");
        if (!int.TryParse(_args[name], NumberStyles.Integer, CultureInfo.InvariantCulture, out int v) || v < min || v > max)
            throw new CommandException($"Argument '{name}' must be a whole number {min}..{max}.");
        return v;
    }

    public double Number(string name, double? fallback = null, double min = double.MinValue, double max = double.MaxValue)
    {
        if (!Has(name)) return fallback ?? throw new CommandException($"Missing argument '{name}'.");
        if (!double.TryParse(_args[name], NumberStyles.Float, CultureInfo.InvariantCulture, out double v) || double.IsNaN(v) || v < min || v > max)
            throw new CommandException($"Argument '{name}' must be a number {min.ToString(CultureInfo.InvariantCulture)}..{max.ToString(CultureInfo.InvariantCulture)}.");
        return v;
    }

    /// <summary>true/false/yes/no/on/off/1/0; "toggle" returns null.</summary>
    public bool? Bool(string name, bool? fallback = null)
    {
        if (!Has(name)) return fallback ?? throw new CommandException($"Missing argument '{name}'.");
        return _args[name].Trim().ToLowerInvariant() switch
        {
            "true" or "yes" or "on" or "1" => true,
            "false" or "no" or "off" or "0" => false,
            "toggle" => null,
            _ => throw new CommandException($"Argument '{name}' must be true, false or toggle."),
        };
    }
}

/// <summary>
/// The single command layer (M7): the UI, macros, automation, the remote API
/// and extensions all call the production through named commands, so every
/// path behaves the same and is logged the same way. Handlers run on the
/// production thread via <see cref="IProductionDispatcher"/>.
/// </summary>
public sealed class CommandService
{
    private readonly Dictionary<string, (CommandDescriptor Descriptor, Func<CommandArgs, Task<string?>> Handler)> _commands =
        new(StringComparer.OrdinalIgnoreCase);
    private readonly object _lock = new();
    private readonly IProductionDispatcher _dispatcher;
    private readonly ILogger<CommandService> _logger;

    public CommandService(IProductionDispatcher dispatcher, ILogger<CommandService> logger)
    {
        _dispatcher = dispatcher;
        _logger = logger;
    }

    /// <summary>Raised after every command (on the production thread).</summary>
    public event EventHandler<(CommandRequest Request, CommandResult Result)>? Executed;

    public IReadOnlyList<CommandDescriptor> Commands
    {
        get
        {
            lock (_lock) return _commands.Values.Select(c => c.Descriptor).OrderBy(d => d.Category).ThenBy(d => d.Name).ToList();
        }
    }

    public bool Contains(string name)
    {
        lock (_lock) return _commands.ContainsKey(name);
    }

    /// <summary>Adds a command. Names are unique (case-insensitive).</summary>
    public void Register(CommandDescriptor descriptor, Func<CommandArgs, Task<string?>> handler)
    {
        ArgumentNullException.ThrowIfNull(descriptor);
        ArgumentNullException.ThrowIfNull(handler);
        if (string.IsNullOrWhiteSpace(descriptor.Name) || descriptor.Name.Any(char.IsWhiteSpace))
            throw new ArgumentException("Command names are non-empty and contain no spaces.", nameof(descriptor));
        lock (_lock)
        {
            if (!_commands.TryAdd(descriptor.Name, (descriptor, handler)))
                throw new InvalidOperationException($"Command '{descriptor.Name}' is already registered.");
        }
    }

    /// <summary>Synchronous convenience for handlers without awaits.</summary>
    public void Register(CommandDescriptor descriptor, Action<CommandArgs> handler) =>
        Register(descriptor, a =>
        {
            handler(a);
            return Task.FromResult<string?>(null);
        });

    /// <summary>Runs a command; never throws for command failures (see the result).</summary>
    public Task<CommandResult> ExecuteAsync(CommandRequest request, string source = "api")
    {
        ArgumentNullException.ThrowIfNull(request);
        (CommandDescriptor Descriptor, Func<CommandArgs, Task<string?>> Handler) entry;
        lock (_lock)
        {
            if (!_commands.TryGetValue(request.Name ?? "", out entry))
                return Task.FromResult(CommandResult.Failure($"Unknown command '{request.Name}'."));
        }
        CommandDescriptor d = entry.Descriptor;
        var args = new CommandArgs(request.Args ?? new Dictionary<string, string>());
        foreach (CommandParameter p in d.Parameters)
        {
            if (p.Required && !args.Has(p.Name))
                return Task.FromResult(CommandResult.Failure($"{d.Name}: missing argument '{p.Name}'."));
        }
        return _dispatcher.InvokeAsync(async () =>
        {
            CommandResult result;
            try
            {
                string? value = await entry.Handler(args).ConfigureAwait(true);
                result = CommandResult.Success(value);
                _logger.LogInformation("Command {Name} ({Source}) {Args}", d.Name, source, Describe(request));
            }
            catch (Exception ex) when (ex is CommandException or InvalidOperationException or ArgumentException
                                           or Interop.MixItNativeException or TimeoutException or IOException
                                           or System.Net.Sockets.SocketException)
            {
                result = CommandResult.Failure($"{d.Name}: {ex.Message}");
                _logger.LogWarning("Command {Name} ({Source}) failed: {Message}", d.Name, source, ex.Message);
            }
            Executed?.Invoke(this, (request, result));
            return result;
        });
    }

    private static string Describe(CommandRequest r) =>
        r.Args is null || r.Args.Count == 0 ? "" : string.Join(' ', r.Args.Select(a => $"{a.Key}={Quote(a.Value)}"));

    private static string Quote(string v) => v.Length <= 64 ? v : v[..61] + "...";
}
