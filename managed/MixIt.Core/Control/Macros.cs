using System.Globalization;
using System.Text;
using Microsoft.Extensions.Logging;
using MixIt.Protocol;

namespace MixIt.Core.Control;

/// <summary>One step of a macro: a command with its arguments.</summary>
public sealed record MacroStep(string Command, IReadOnlyDictionary<string, string> Args)
{
    public CommandRequest ToRequest() => new(Command, Args);
}

/// <summary>A named list of commands run in order (M7).</summary>
public sealed record Macro(string Name, IReadOnlyList<MacroStep> Steps, bool StopOnError = true)
{
    /// <summary>
    /// Parses the macro text format: one command per line,
    /// <c>Command key=value key="value with spaces"</c>; <c>#</c> starts a comment;
    /// <c>Wait 500</c> is short for <c>Wait ms=500</c>.
    /// </summary>
    public static Macro Parse(string name, string text, bool stopOnError = true)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(name);
        ArgumentNullException.ThrowIfNull(text);
        var steps = new List<MacroStep>();
        string[] lines = text.Replace("\r\n", "\n", StringComparison.Ordinal).Split('\n');
        for (int n = 0; n < lines.Length; n++)
        {
            string line = lines[n].Trim();
            if (line.Length == 0 || line.StartsWith('#')) continue;
            try
            {
                steps.Add(ParseLine(line));
            }
            catch (FormatException ex)
            {
                throw new FormatException($"Line {n + 1}: {ex.Message}");
            }
        }
        if (steps.Count > 1000) throw new FormatException("A macro has at most 1000 steps.");
        return new Macro(name.Trim(), steps, stopOnError);
    }

    private static MacroStep ParseLine(string line)
    {
        List<string> tokens = Tokenize(line);
        string command = tokens[0];
        if (command.Contains('=', StringComparison.Ordinal)) throw new FormatException("A line starts with a command name.");
        var args = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        for (int i = 1; i < tokens.Count; i++)
        {
            int eq = tokens[i].IndexOf('=', StringComparison.Ordinal);
            if (eq <= 0)
            {
                if (tokens.Count == 2 && command.Equals("Wait", StringComparison.OrdinalIgnoreCase))
                {
                    args["ms"] = tokens[i];
                    continue;
                }
                throw new FormatException($"Expected key=value, got '{tokens[i]}'.");
            }
            args[tokens[i][..eq]] = tokens[i][(eq + 1)..];
        }
        return new MacroStep(command, args);
    }

    private static List<string> Tokenize(string line)
    {
        var tokens = new List<string>();
        var sb = new StringBuilder();
        bool quoted = false;
        for (int i = 0; i < line.Length; i++)
        {
            char c = line[i];
            if (c == '"')
            {
                quoted = !quoted;
                continue;
            }
            if (c == '\\' && quoted && i + 1 < line.Length && line[i + 1] is '"' or '\\')
            {
                sb.Append(line[++i]);
                continue;
            }
            if (char.IsWhiteSpace(c) && !quoted)
            {
                if (sb.Length > 0) tokens.Add(sb.ToString());
                sb.Clear();
                continue;
            }
            sb.Append(c);
        }
        if (quoted) throw new FormatException("Unclosed quote.");
        if (sb.Length > 0) tokens.Add(sb.ToString());
        return tokens;
    }

    /// <summary>The text form (round-trips through <see cref="Parse"/>).</summary>
    public string ToText()
    {
        var sb = new StringBuilder();
        foreach (MacroStep s in Steps)
        {
            sb.Append(s.Command);
            foreach ((string k, string v) in s.Args)
            {
                sb.Append(' ').Append(k).Append('=');
                if (v.Length == 0 || v.Any(ch => char.IsWhiteSpace(ch) || ch is '"' or '\\'))
                    sb.Append('"').Append(v.Replace("\\", "\\\\", StringComparison.Ordinal).Replace("\"", "\\\"", StringComparison.Ordinal)).Append('"');
                else
                    sb.Append(v);
            }
            sb.AppendLine();
        }
        return sb.ToString();
    }
}

/// <summary>Result of a macro run.</summary>
public sealed record MacroRun(string Macro, int StepsRun, int StepCount, bool Cancelled, string? Error)
{
    public bool Ok => Error is null && !Cancelled;
}

/// <summary>
/// Runs macros through the command layer (each step as a normal command), one
/// step at a time. Several macros may run at once; each can be cancelled.
/// Registers <c>RunMacro</c> and <c>StopMacros</c>.
/// </summary>
public sealed class MacroRunner
{
    private const int MaxNesting = 8;
    private const int MaxConcurrent = 32;
    private static readonly AsyncLocal<int> Depth = new();
    private readonly CommandService _commands;
    private readonly ILogger<MacroRunner> _logger;
    private readonly Dictionary<string, Macro> _macros = new(StringComparer.OrdinalIgnoreCase);
    private readonly object _lock = new();
    private CancellationTokenSource _stopAll = new();
    private int _running;

    public MacroRunner(CommandService commands, ILogger<MacroRunner> logger)
    {
        _commands = commands;
        _logger = logger;
    }

    /// <summary>Raised when a macro finishes (any thread).</summary>
    public event EventHandler<MacroRun>? Finished;

    public IReadOnlyList<Macro> Macros
    {
        get
        {
            lock (_lock) return _macros.Values.OrderBy(m => m.Name, StringComparer.OrdinalIgnoreCase).ToList();
        }
    }

    public int RunningCount => Volatile.Read(ref _running);

    public void SetMacros(IEnumerable<Macro> macros)
    {
        lock (_lock)
        {
            _macros.Clear();
            foreach (Macro m in macros)
            {
                if (!_macros.TryAdd(m.Name, m)) throw new ArgumentException($"Two macros are named '{m.Name}'.");
            }
        }
    }

    public void Save(Macro macro)
    {
        ArgumentNullException.ThrowIfNull(macro);
        lock (_lock) _macros[macro.Name] = macro;
    }

    public bool Delete(string name)
    {
        lock (_lock) return _macros.Remove(name);
    }

    public Macro Find(string name)
    {
        lock (_lock)
        {
            return _macros.TryGetValue(name, out Macro? m) ? m : throw new CommandException($"No macro '{name}'.");
        }
    }

    /// <summary>Runs a macro by name to the end (or the first error when it stops on errors).</summary>
    public Task<MacroRun> RunAsync(string name, CancellationToken cancellationToken = default) => RunAsync(Find(name), cancellationToken);

    public async Task<MacroRun> RunAsync(Macro macro, CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(macro);
        if (Depth.Value >= MaxNesting) return Done(new MacroRun(macro.Name, 0, macro.Steps.Count, false, $"Macros nested deeper than {MaxNesting} (a macro calling itself?)."));
        CancellationToken stopAll;
        lock (_lock) stopAll = _stopAll.Token;
        using var linked = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken, stopAll);
        Interlocked.Increment(ref _running);
        Depth.Value++;
        int done = 0;
        string? error = null;
        try
        {
            _logger.LogInformation("Macro {Name} started ({Steps} steps)", macro.Name, macro.Steps.Count);
            foreach (MacroStep step in macro.Steps)
            {
                if (linked.IsCancellationRequested) break;
                CommandResult r = step.Command.Equals("Wait", StringComparison.OrdinalIgnoreCase)
                    ? await WaitAsync(step, linked.Token).ConfigureAwait(false)
                    : await _commands.ExecuteAsync(step.ToRequest(), $"macro {macro.Name}").ConfigureAwait(false);
                done++;
                if (!r.Ok && macro.StopOnError)
                {
                    error = $"step {done} ({step.Command}): {r.Error}";
                    break;
                }
            }
        }
        finally
        {
            Depth.Value--;
            Interlocked.Decrement(ref _running);
        }
        bool cancelled = error is null && done < macro.Steps.Count;
        return Done(new MacroRun(macro.Name, done, macro.Steps.Count, cancelled, error));
    }

    /// <summary>Waits run here (off the production thread) and are cancellable.</summary>
    private static async Task<CommandResult> WaitAsync(MacroStep step, CancellationToken token)
    {
        int ms;
        try
        {
            ms = new CommandArgs(step.Args).Int("ms", null, 0, 600_000);
        }
        catch (CommandException ex)
        {
            return CommandResult.Failure(ex.Message);
        }
        try
        {
            await Task.Delay(ms, token).ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            // cancelled: the loop notices
        }
        return CommandResult.Success();
    }

    /// <summary>Cancels every running macro (the current step finishes).</summary>
    public void StopAll()
    {
        CancellationTokenSource old;
        lock (_lock)
        {
            old = _stopAll;
            _stopAll = new CancellationTokenSource();
        }
        old.Cancel();
        old.Dispose();
    }

    private MacroRun Done(MacroRun run)
    {
        if (run.Error is not null) _logger.LogWarning("Macro {Name} stopped at {Error}", run.Macro, run.Error);
        else _logger.LogInformation("Macro {Name} {Outcome} ({Done}/{Count} steps)", run.Macro, run.Cancelled ? "cancelled" : "finished", run.StepsRun, run.StepCount);
        Finished?.Invoke(this, run);
        return run;
    }

    public void Register()
    {
        _commands.Register(new("RunMacro", "Run a macro (returns when it is started).",
            [new("name", ParameterKind.String, true, "Macro name"), new("wait", ParameterKind.Boolean, false, "true: return when it has finished")], "Macros"), async a =>
        {
            Macro m = Find(a.String("name"));
            if (a.Bool("wait", false) == true)
            {
                MacroRun r = await RunAsync(m).ConfigureAwait(true);
                if (!r.Ok) throw new CommandException(r.Error ?? "cancelled");
                return r.StepsRun.ToString(CultureInfo.InvariantCulture);
            }
            if (RunningCount >= MaxConcurrent) throw new CommandException($"{MaxConcurrent} macros are already running.");
            // Started off the production thread (without this command's context) so
            // its steps queue behind this command like any other caller's.
            using (ExecutionContext.SuppressFlow()) _ = Task.Run(() => RunAsync(m));
            return null;
        });
        _commands.Register(new("StopMacros", "Cancel every running macro.", [], "Macros"), _ => StopAll());
    }
}
