using System.Text.Json;
using System.Text.Json.Serialization;
using Microsoft.Extensions.Logging;

namespace MixIt.Core.Control;

/// <summary>When an automation rule fires.</summary>
public enum TriggerKind
{
    /// <summary>Every day at <see cref="AutomationRule.TimeOfDay"/> (local time).</summary>
    DailyAt,
    /// <summary>Once at <see cref="AutomationRule.At"/> (local time).</summary>
    Once,
    /// <summary>Every <see cref="AutomationRule.IntervalSeconds"/> while armed.</summary>
    Interval,
    /// <summary>When the production starts.</summary>
    ProductionStarted,
    /// <summary>When the production stops.</summary>
    ProductionStopped,
}

/// <summary>A rule that runs a macro (M7).</summary>
public sealed record AutomationRule
{
    public required string Name { get; init; }
    public required TriggerKind Trigger { get; init; }
    public required string Macro { get; init; }
    public bool Enabled { get; init; } = true;
    public TimeOnly? TimeOfDay { get; init; }
    public DateTime? At { get; init; }
    public int IntervalSeconds { get; init; }

    public void Validate()
    {
        if (string.IsNullOrWhiteSpace(Name)) throw new ArgumentException("A rule needs a name.");
        if (string.IsNullOrWhiteSpace(Macro)) throw new ArgumentException($"Rule '{Name}' needs a macro.");
        switch (Trigger)
        {
            case TriggerKind.DailyAt when TimeOfDay is null:
                throw new ArgumentException($"Rule '{Name}': daily rules need a time.");
            case TriggerKind.Once when At is null:
                throw new ArgumentException($"Rule '{Name}': one-time rules need a date and time.");
            case TriggerKind.Interval when IntervalSeconds is < 5 or > 86_400:
                throw new ArgumentException($"Rule '{Name}': interval must be 5 s .. 24 h.");
        }
    }

    public string Describe() => Trigger switch
    {
        TriggerKind.DailyAt => $"every day at {TimeOfDay:HH\\:mm\\:ss}",
        TriggerKind.Once => $"once at {At:yyyy-MM-dd HH:mm:ss}",
        TriggerKind.Interval => $"every {IntervalSeconds} s",
        TriggerKind.ProductionStarted => "when production starts",
        _ => "when production stops",
    };
}

/// <summary>
/// Fires automation rules (M7). Nothing fires unless the operator has
/// <see cref="Armed"/> automation (CLAUDE.md: no unattended changes during a
/// broadcast without approval). Call <see cref="Tick"/> about once a second
/// and the production hooks on start/stop; the clock is injectable for tests.
/// </summary>
public sealed class AutomationScheduler
{
    private readonly MacroRunner _macros;
    private readonly ILogger<AutomationScheduler> _logger;
    private readonly Func<DateTime> _now;
    private List<AutomationRule> _rules = [];
    private readonly Dictionary<string, DateTime> _lastFired = new(StringComparer.OrdinalIgnoreCase);
    private DateTime _lastTick;
    private bool _armed;

    public AutomationScheduler(MacroRunner macros, ILogger<AutomationScheduler> logger, Func<DateTime>? now = null)
    {
        _macros = macros;
        _logger = logger;
        _now = now ?? (() => DateTime.Now);
        _lastTick = _now();
    }

    /// <summary>Raised when a rule fires (before its macro runs).</summary>
    public event EventHandler<AutomationRule>? Fired;

    public IReadOnlyList<AutomationRule> Rules => _rules;

    public bool Armed
    {
        get => _armed;
        set
        {
            if (_armed == value) return;
            _armed = value;
            _lastTick = _now();  // never catch up on times that passed while disarmed
            _lastFired.Clear();
            _logger.LogInformation("Automation {State}", value ? "armed" : "disarmed");
        }
    }

    public void SetRules(IEnumerable<AutomationRule> rules)
    {
        var list = rules.ToList();
        foreach (AutomationRule r in list) r.Validate();
        if (list.Select(r => r.Name).Distinct(StringComparer.OrdinalIgnoreCase).Count() != list.Count)
            throw new ArgumentException("Rule names must be unique.");
        _rules = list;
        _lastFired.Clear();
    }

    /// <summary>Checks time-based rules for the period since the previous tick.</summary>
    public void Tick()
    {
        DateTime now = _now();
        DateTime from = _lastTick;
        _lastTick = now;
        if (!_armed || now <= from) return;
        foreach (AutomationRule r in _rules.Where(r => r.Enabled))
        {
            if (IsDue(r, from, now)) Fire(r, now);
        }
    }

    private bool IsDue(AutomationRule r, DateTime from, DateTime now)
    {
        switch (r.Trigger)
        {
            case TriggerKind.DailyAt:
                return CrossesTimeOfDay(from, now, r.TimeOfDay!.Value);
            case TriggerKind.Once:
                return r.At > from && r.At <= now;
            case TriggerKind.Interval:
                if (_lastFired.TryGetValue(r.Name, out DateTime last)) return now - last >= TimeSpan.FromSeconds(r.IntervalSeconds);
                _lastFired[r.Name] = now;  // first tick after arming: start counting
                return false;
            default:
                return false;
        }
    }

    public void ProductionStarted() => FireEvent(TriggerKind.ProductionStarted);

    public void ProductionStopped() => FireEvent(TriggerKind.ProductionStopped);

    private void FireEvent(TriggerKind kind)
    {
        if (!_armed) return;
        foreach (AutomationRule r in _rules.Where(r => r.Enabled && r.Trigger == kind)) Fire(r, _now());
    }

    private static bool CrossesTimeOfDay(DateTime from, DateTime to, TimeOnly time)
    {
        // Every occurrence of `time` in (from, to]; ticks are ~1 s apart, but a
        // sleeping PC may jump: fire once, not for every missed day.
        DateTime candidate = from.Date + time.ToTimeSpan();
        if (candidate <= from) candidate = candidate.AddDays(1);
        return candidate <= to && to - candidate < TimeSpan.FromMinutes(1);
    }

    private void Fire(AutomationRule rule, DateTime now)
    {
        _lastFired[rule.Name] = now;
        _logger.LogInformation("Automation rule {Rule} ({Trigger}) runs macro {Macro}", rule.Name, rule.Describe(), rule.Macro);
        Fired?.Invoke(this, rule);
        _ = RunAsync(rule);
    }

    private async Task RunAsync(AutomationRule rule)
    {
        try
        {
            MacroRun run = await _macros.RunAsync(rule.Macro).ConfigureAwait(false);
            if (!run.Ok) _logger.LogWarning("Automation rule {Rule}: macro {Macro} did not complete: {Error}", rule.Name, rule.Macro, run.Error ?? "cancelled");
        }
        catch (CommandException ex)
        {
            _logger.LogWarning("Automation rule {Rule}: {Message}", rule.Name, ex.Message);
        }
    }
}

/// <summary>Remote-control server settings.</summary>
public sealed record RemoteSettings
{
    public bool Enabled { get; init; }
    public int Port { get; init; } = Protocol.ProtocolInfo.DefaultPort;
    /// <summary>false: localhost only. true: every network interface (LAN control surfaces; firewall prompt).</summary>
    public bool AllowNetwork { get; init; }
    /// <summary>Bearer token every request must carry. Generated on first use; never logged.</summary>
    public string Token { get; init; } = "";
}

/// <summary>Everything the operator configures for M7, saved as one JSON file.</summary>
public sealed record ControlSettings
{
    public List<MacroDocument> Macros { get; init; } = [];
    public List<AutomationRule> Rules { get; init; } = [];
    public List<TallyDestination> Tally { get; init; } = [];
    public List<PtzCamera> Cameras { get; init; } = [];
    public RemoteSettings Remote { get; init; } = new();
    public bool ExtensionsEnabled { get; init; }
}

/// <summary>A macro as stored: its text form (readable and editable).</summary>
public sealed record MacroDocument(string Name, string Text, bool StopOnError = true);

/// <summary>Loads and saves <see cref="ControlSettings"/> (atomic replace; a broken file is kept aside, never lost).</summary>
public sealed class ControlSettingsStore(string path, ILogger<ControlSettingsStore> logger)
{
    private static readonly JsonSerializerOptions Options = new(JsonSerializerDefaults.Web)
    {
        WriteIndented = true,
        // Files for people to read (not HTML): keep "&", quotes and accents readable.
        Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
        Converters = { new JsonStringEnumConverter() },
    };

    public string Path { get; } = path;

    public static string DefaultPath() => System.IO.Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "MIX IT", "control.json");

    public ControlSettings Load()
    {
        if (!File.Exists(Path)) return new ControlSettings();
        try
        {
            return JsonSerializer.Deserialize<ControlSettings>(File.ReadAllText(Path), Options) ?? new ControlSettings();
        }
        catch (JsonException ex)
        {
            string aside = Path + $".broken-{DateTime.Now:yyyyMMdd-HHmmss}";
            File.Move(Path, aside);
            logger.LogError("Control settings could not be read ({Message}); kept as {File}, starting empty", ex.Message, aside);
            return new ControlSettings();
        }
    }

    public void Save(ControlSettings settings)
    {
        ArgumentNullException.ThrowIfNull(settings);
        Directory.CreateDirectory(System.IO.Path.GetDirectoryName(Path)!);
        string temp = Path + ".tmp";
        File.WriteAllText(temp, JsonSerializer.Serialize(settings, Options));
        File.Move(temp, Path, overwrite: true);
    }
}
