using System.Collections.ObjectModel;
using System.Globalization;
using System.Windows;
using System.Windows.Input;
using System.Windows.Threading;
using Microsoft.Extensions.Logging;
using MixIt.Core.Control;
using MixIt.Desktop.Services;
using MixIt.Protocol;
using MixIt.Remote;

namespace MixIt.Desktop.ViewModels;

/// <summary>
/// Automation &amp; Control (M7): macros, automation rules, remote control,
/// tally destinations, PTZ cameras and extensions. Everything goes through
/// <see cref="ControlHost"/>; this class only binds.
/// </summary>
public sealed class ControlViewModel : ObservableObject
{
    private readonly ControlHost _host;
    private readonly ILogger<ControlViewModel> _logger;
    private readonly DispatcherTimer _timer;
    private string? _error;
    private string _status = "";
    private Macro? _selectedMacro;
    private string _macroName = "";
    private string _macroText = "";
    private bool _stopOnError = true;
    private AutomationRule? _selectedRule;
    private string _ruleName = "";
    private TriggerKind _ruleTrigger = TriggerKind.DailyAt;
    private string _ruleWhen = "19:00";
    private string? _ruleMacro;
    private string _tallyHost = "";
    private string _tallyPort = "8900";
    private string _tallyScreen = "0";
    private TallyDestination? _selectedTally;
    private string _cameraName = "";
    private string _cameraHost = "";
    private string _cameraPort = "52381";
    private ViscaTransport _cameraTransport = ViscaTransport.SonyUdp;
    private PtzCamera? _selectedCamera;
    private bool _storePresets;
    private string _remotePort = "";
    private bool _tokenVisible;
    private bool _active;

    public ControlViewModel(ControlHost host, ILogger<ControlViewModel> logger)
    {
        _host = host;
        _logger = logger;
        _remotePort = host.Settings.Remote.Port.ToString(CultureInfo.InvariantCulture);
        NewMacroCommand = new RelayCommand(_ => SelectedMacro = null);
        SaveMacroCommand = new RelayCommand(_ => Guard(SaveMacro), _ => _macroName.Trim().Length > 0);
        DeleteMacroCommand = new RelayCommand(_ => Guard(() =>
        {
            _host.Macros.Delete(_selectedMacro!.Name);
            _host.Save();
            Refresh();
        }), _ => _selectedMacro is not null);
        RunMacroCommand = new RelayCommand(_ => Guard(() => _ = RunMacroAsync(_selectedMacro!)), _ => _selectedMacro is not null);
        StopMacrosCommand = new RelayCommand(_ => _host.Macros.StopAll());
        AddRuleCommand = new RelayCommand(_ => Guard(AddRule), _ => _ruleName.Trim().Length > 0 && _ruleMacro is not null);
        RemoveRuleCommand = new RelayCommand(_ => Guard(() =>
        {
            _host.Automation.SetRules(_host.Automation.Rules.Where(r => r != _selectedRule));
            _host.Save();
            Refresh();
        }), _ => _selectedRule is not null);
        AddTallyCommand = new RelayCommand(_ => Guard(AddTally), _ => _tallyHost.Trim().Length > 0);
        RemoveTallyCommand = new RelayCommand(_ => Guard(() =>
        {
            _host.Tally.SetDestinations(_host.Tally.Destinations.Where(d => d != _selectedTally));
            _host.Save();
            Refresh();
        }), _ => _selectedTally is not null);
        AddCameraCommand = new RelayCommand(_ => Guard(AddCamera), _ => _cameraName.Trim().Length > 0 && _cameraHost.Trim().Length > 0);
        RemoveCameraCommand = new RelayCommand(_ => Guard(() =>
        {
            _host.Ptz.SetCameras(_host.Ptz.Cameras.Where(c => c != _selectedCamera));
            _host.Save();
            Refresh();
        }), _ => _selectedCamera is not null);
        PtzPresetCommand = new RelayCommand(p => Ptz(_storePresets ? "PtzStorePreset" : "PtzPreset", ("preset", (string)p!)), _ => _selectedCamera is not null);
        PtzMoveCommand = new RelayCommand(p => Ptz("PtzMove", ("direction", (string)p!)), _ => _selectedCamera is not null);
        PtzStopCommand = new RelayCommand(_ => Ptz("PtzStop"), _ => _selectedCamera is not null);
        PtzZoomCommand = new RelayCommand(p => Ptz("PtzZoom", ("direction", (string)p!)), _ => _selectedCamera is not null);
        PtzHomeCommand = new RelayCommand(_ => Ptz("PtzHome"), _ => _selectedCamera is not null);
        ApplyRemoteCommand = new AsyncCommand(ApplyRemoteAsync);
        NewTokenCommand = new AsyncCommand(async () =>
        {
            _host.Update(s => s with { Remote = s.Remote with { Token = RemoteControlServer.NewToken() } });
            OnPropertyChanged(nameof(Token));
            if (_host.Remote.IsRunning) await _host.StartRemoteAsync().ConfigureAwait(true);
            Refresh();
        });
        CopyTokenCommand = new RelayCommand(_ => Clipboard.SetText(_host.Settings.Remote.Token));
        _timer = new DispatcherTimer(DispatcherPriority.Background) { Interval = TimeSpan.FromMilliseconds(500) };
        _timer.Tick += (_, _) => RefreshStatus();
        _host.Macros.Finished += (_, run) => Application.Current?.Dispatcher.InvokeAsync(() =>
            Status = run.Ok ? $"Macro {run.Macro} finished ({run.StepsRun} steps)." : $"Macro {run.Macro}: {run.Error ?? "cancelled"}");
        _host.Automation.Fired += (_, r) => Status = $"Automation: {r.Name} → {r.Macro}";
        Refresh();
    }

    public ObservableCollection<Macro> Macros { get; } = [];
    public ObservableCollection<AutomationRule> Rules { get; } = [];
    public ObservableCollection<TallyDestination> TallyDestinations { get; } = [];
    public ObservableCollection<PtzCamera> Cameras { get; } = [];
    public ObservableCollection<LoadedExtension> Extensions { get; } = [];
    public ObservableCollection<string> MacroNames { get; } = [];
    public IReadOnlyList<CommandDescriptor> CommandReference => _host.Commands.Commands;
    public IReadOnlyList<TriggerKind> Triggers { get; } = Enum.GetValues<TriggerKind>();
    public IReadOnlyList<ViscaTransport> Transports { get; } = Enum.GetValues<ViscaTransport>();
    public IReadOnlyList<string> Presets { get; } = Enumerable.Range(1, 8).Select(i => i.ToString(CultureInfo.InvariantCulture)).ToList();

    public ICommand NewMacroCommand { get; }
    public ICommand SaveMacroCommand { get; }
    public ICommand DeleteMacroCommand { get; }
    public ICommand RunMacroCommand { get; }
    public ICommand StopMacrosCommand { get; }
    public ICommand AddRuleCommand { get; }
    public ICommand RemoveRuleCommand { get; }
    public ICommand AddTallyCommand { get; }
    public ICommand RemoveTallyCommand { get; }
    public ICommand AddCameraCommand { get; }
    public ICommand RemoveCameraCommand { get; }
    public ICommand PtzPresetCommand { get; }
    public ICommand PtzMoveCommand { get; }
    public ICommand PtzStopCommand { get; }
    public ICommand PtzZoomCommand { get; }
    public ICommand PtzHomeCommand { get; }
    public ICommand ApplyRemoteCommand { get; }
    public ICommand NewTokenCommand { get; }
    public ICommand CopyTokenCommand { get; }

    public string? Error
    {
        get => _error;
        private set => SetProperty(ref _error, value);
    }

    public string Status
    {
        get => _status;
        private set => SetProperty(ref _status, value);
    }

    public bool IsActive
    {
        get => _active;
        set
        {
            if (!SetProperty(ref _active, value)) return;
            if (value)
            {
                Refresh();
                _timer.Start();
            }
            else
            {
                _timer.Stop();
            }
        }
    }

    // ---- macros

    public Macro? SelectedMacro
    {
        get => _selectedMacro;
        set
        {
            if (!SetProperty(ref _selectedMacro, value)) return;
            MacroName = value?.Name ?? "";
            MacroText = value?.ToText() ?? "";
            StopOnError = value?.StopOnError ?? true;
        }
    }

    public string MacroName
    {
        get => _macroName;
        set => SetProperty(ref _macroName, value ?? "");
    }

    public string MacroText
    {
        get => _macroText;
        set => SetProperty(ref _macroText, value ?? "");
    }

    public bool StopOnError
    {
        get => _stopOnError;
        set => SetProperty(ref _stopOnError, value);
    }

    private void SaveMacro()
    {
        Macro m = Macro.Parse(_macroName, _macroText, _stopOnError);  // FormatException names the line
        foreach (MacroStep step in m.Steps)
        {
            if (!_host.Commands.Contains(step.Command)) throw new ArgumentException($"Unknown command '{step.Command}'.");
        }
        if (_selectedMacro is not null && !_selectedMacro.Name.Equals(m.Name, StringComparison.OrdinalIgnoreCase)) _host.Macros.Delete(_selectedMacro.Name);
        _host.Macros.Save(m);
        _host.Save();
        Refresh();
        SelectedMacro = Macros.FirstOrDefault(x => x.Name == m.Name);
        Status = $"Macro {m.Name} saved ({m.Steps.Count} steps).";
    }

    internal async Task<MacroRun> RunMacroAsync(Macro macro)
    {
        Status = $"Running {macro.Name}…";
        return await _host.Macros.RunAsync(macro).ConfigureAwait(true);
    }

    // ---- automation

    public bool Armed
    {
        get => _host.Automation.Armed;
        set
        {
            _host.Automation.Armed = value;
            OnPropertyChanged();
        }
    }

    public AutomationRule? SelectedRule
    {
        get => _selectedRule;
        set => SetProperty(ref _selectedRule, value);
    }

    public string RuleName
    {
        get => _ruleName;
        set => SetProperty(ref _ruleName, value ?? "");
    }

    public TriggerKind RuleTrigger
    {
        get => _ruleTrigger;
        set => SetProperty(ref _ruleTrigger, value);
    }

    /// <summary>Daily: HH:mm[:ss]; once: yyyy-MM-dd HH:mm; interval: seconds.</summary>
    public string RuleWhen
    {
        get => _ruleWhen;
        set => SetProperty(ref _ruleWhen, value ?? "");
    }

    public string? RuleMacro
    {
        get => _ruleMacro;
        set => SetProperty(ref _ruleMacro, value);
    }

    private void AddRule()
    {
        CultureInfo inv = CultureInfo.InvariantCulture;
        var rule = new AutomationRule { Name = _ruleName.Trim(), Trigger = _ruleTrigger, Macro = _ruleMacro! };
        rule = _ruleTrigger switch
        {
            TriggerKind.DailyAt => rule with { TimeOfDay = TimeOnly.TryParse(_ruleWhen, inv, out TimeOnly t) ? t : throw new ArgumentException("Time as HH:mm.") },
            TriggerKind.Once => rule with { At = DateTime.TryParse(_ruleWhen, inv, DateTimeStyles.AssumeLocal, out DateTime d) ? d : throw new ArgumentException("Date and time as yyyy-MM-dd HH:mm.") },
            TriggerKind.Interval => rule with { IntervalSeconds = int.TryParse(_ruleWhen, NumberStyles.None, inv, out int s) ? s : throw new ArgumentException("Interval in seconds.") },
            _ => rule,
        };
        _host.Automation.SetRules(_host.Automation.Rules.Where(r => !r.Name.Equals(rule.Name, StringComparison.OrdinalIgnoreCase)).Append(rule));
        _host.Save();
        Refresh();
        Status = $"Rule {rule.Name}: {rule.Describe()} → {rule.Macro}" + (Armed ? "" : " (automation is not armed)");
    }

    // ---- remote

    public bool RemoteEnabled
    {
        get => _host.Settings.Remote.Enabled;
        set => _host.Update(s => s with { Remote = s.Remote with { Enabled = value } });
    }

    public bool RemoteNetwork
    {
        get => _host.Settings.Remote.AllowNetwork;
        set => _host.Update(s => s with { Remote = s.Remote with { AllowNetwork = value } });
    }

    public string RemotePort
    {
        get => _remotePort;
        set => SetProperty(ref _remotePort, value ?? "");
    }

    public bool TokenVisible
    {
        get => _tokenVisible;
        set
        {
            if (SetProperty(ref _tokenVisible, value)) OnPropertyChanged(nameof(Token));
        }
    }

    public string Token => _tokenVisible ? _host.Settings.Remote.Token : new string('•', 16);

    public string RemoteState => _host.Remote.IsRunning
        ? $"Listening — {_host.Remote.Address}, {_host.Remote.ClientCount} live client(s). Control page: open the address in a browser."
        : _host.RemoteError ?? "Off";

    private async Task ApplyRemoteAsync()
    {
        Error = null;
        if (!int.TryParse(_remotePort, NumberStyles.None, CultureInfo.InvariantCulture, out int port) || port is < 1024 or > 65535)
        {
            Error = "Port must be 1024..65535.";
            return;
        }
        _host.Update(s => s with { Remote = s.Remote with { Port = port } });
        if (_host.Settings.Remote.Enabled) await _host.StartRemoteAsync().ConfigureAwait(true);
        else await _host.Remote.StopAsync().ConfigureAwait(true);
        if (_host.RemoteError is not null) Error = _host.RemoteError;
        OnPropertyChanged(nameof(RemoteState));
    }

    // ---- tally

    public string TallyHost
    {
        get => _tallyHost;
        set => SetProperty(ref _tallyHost, value ?? "");
    }

    public string TallyPort
    {
        get => _tallyPort;
        set => SetProperty(ref _tallyPort, value ?? "");
    }

    public string TallyScreen
    {
        get => _tallyScreen;
        set => SetProperty(ref _tallyScreen, value ?? "");
    }

    public TallyDestination? SelectedTally
    {
        get => _selectedTally;
        set => SetProperty(ref _selectedTally, value);
    }

    public string TallySummary => string.Join("   ", _host.Tally.Current.Select(t => $"{t.Number} {(t.Program ? "● PGM" : t.Preview ? "● PVW" : "○")}"));

    private void AddTally()
    {
        var d = new TallyDestination(_tallyHost.Trim(), ParseInt(_tallyPort, "Port"), ParseInt(_tallyScreen, "Screen"));
        _host.Tally.SetDestinations(_host.Tally.Destinations.Append(d));
        _host.Save();
        Refresh();
    }

    // ---- PTZ

    public string CameraName
    {
        get => _cameraName;
        set => SetProperty(ref _cameraName, value ?? "");
    }

    public string CameraHost
    {
        get => _cameraHost;
        set => SetProperty(ref _cameraHost, value ?? "");
    }

    public string CameraPort
    {
        get => _cameraPort;
        set => SetProperty(ref _cameraPort, value ?? "");
    }

    public ViscaTransport CameraTransport
    {
        get => _cameraTransport;
        set
        {
            if (!SetProperty(ref _cameraTransport, value)) return;
            CameraPort = value switch { ViscaTransport.SonyUdp => "52381", ViscaTransport.RawUdp => "1259", _ => "5678" };
        }
    }

    public PtzCamera? SelectedCamera
    {
        get => _selectedCamera;
        set => SetProperty(ref _selectedCamera, value);
    }

    /// <summary>When on, preset buttons store instead of recall.</summary>
    public bool StorePresets
    {
        get => _storePresets;
        set => SetProperty(ref _storePresets, value);
    }

    private void AddCamera()
    {
        var c = new PtzCamera(_cameraName.Trim(), _cameraHost.Trim(), ParseInt(_cameraPort, "Port"), _cameraTransport);
        _host.Ptz.SetCameras(_host.Ptz.Cameras.Append(c));
        _host.Save();
        Refresh();
        SelectedCamera = Cameras.FirstOrDefault(x => x.Name == c.Name);
    }

    private void Ptz(string command, params (string, string)[] args)
    {
        if (_selectedCamera is null) return;
        var all = args.Append(("camera", _selectedCamera.Name)).ToArray();
        _ = RunCommandAsync(CommandRequest.Of(command, all));
        if (command == "PtzStorePreset") StorePresets = false;  // one store per arming, to avoid overwriting by accident
    }

    private async Task RunCommandAsync(CommandRequest request)
    {
        CommandResult r = await _host.Commands.ExecuteAsync(request, "ui").ConfigureAwait(true);
        Error = r.Ok ? null : r.Error;
    }

    // ---- extensions

    public bool ExtensionsEnabled
    {
        get => _host.Settings.ExtensionsEnabled;
        set
        {
            _host.Update(s => s with { ExtensionsEnabled = value });
            OnPropertyChanged();
            Status = "Extensions load at the next start of MIX IT.";
        }
    }

    public string ExtensionsFolder => ExtensionManager.DefaultFolder();

    // ---- shared

    private static int ParseInt(string text, string what) =>
        int.TryParse(text, NumberStyles.None, CultureInfo.InvariantCulture, out int v) ? v : throw new ArgumentException($"{what} must be a number.");

    private void Guard(Action action)
    {
        Error = null;
        try
        {
            action();
        }
        catch (Exception ex) when (ex is FormatException or ArgumentException or InvalidOperationException or CommandException)
        {
            _logger.LogWarning("Control operation failed: {Message}", ex.Message);
            Error = ex.Message;
        }
    }

    private void Refresh()
    {
        string? macro = _selectedMacro?.Name;
        Macros.Clear();
        MacroNames.Clear();
        foreach (Macro m in _host.Macros.Macros)
        {
            Macros.Add(m);
            MacroNames.Add(m.Name);
        }
        _selectedMacro = Macros.FirstOrDefault(m => m.Name == macro);
        OnPropertyChanged(nameof(SelectedMacro));
        RuleMacro ??= MacroNames.FirstOrDefault();
        Rules.Clear();
        foreach (AutomationRule r in _host.Automation.Rules) Rules.Add(r);
        TallyDestinations.Clear();
        foreach (TallyDestination d in _host.Tally.Destinations) TallyDestinations.Add(d);
        string? camera = _selectedCamera?.Name;
        Cameras.Clear();
        foreach (PtzCamera c in _host.Ptz.Cameras) Cameras.Add(c);
        SelectedCamera = Cameras.FirstOrDefault(c => c.Name == camera) ?? Cameras.FirstOrDefault();
        Extensions.Clear();
        foreach (LoadedExtension e in _host.Extensions.Extensions) Extensions.Add(e);
        OnPropertyChanged(nameof(CommandReference));
        RefreshStatus();
    }

    private void RefreshStatus()
    {
        OnPropertyChanged(nameof(RemoteState));
        OnPropertyChanged(nameof(TallySummary));
        OnPropertyChanged(nameof(Armed));
    }
}
