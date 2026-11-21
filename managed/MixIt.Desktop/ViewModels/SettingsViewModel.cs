using System.Collections.ObjectModel;
using System.IO;
using System.Text;
using System.Windows;
using System.Windows.Input;
using Microsoft.Extensions.Logging;
using Microsoft.Win32;
using MixIt.Core.Control;
using MixIt.Core.Production;
using MixIt.Desktop.Services;
using MixIt.Interop;
using MixIt.Protocol;

namespace MixIt.Desktop.ViewModels;

public sealed record BusChoice(string Title, VideoBus Bus);

/// <summary>
/// Settings &amp; workspace (M8): show files (new/open/save), keyboard shortcuts
/// (each runs a command, same syntax as macros), full-screen outputs on other
/// displays, and where MIX IT keeps its files.
/// </summary>
public sealed class SettingsViewModel : ObservableObject
{
    private readonly ProductionService _production;
    private readonly ControlHost _control;
    private readonly WorkspaceStore _store;
    private readonly ILogger<SettingsViewModel> _logger;
    private readonly ISecretProtector _protector = new DpapiSecretProtector();
    private WorkspaceSettings _workspace;
    private string _shortcutText;
    private string? _error;
    private string _status = "";
    private string? _showPath;
    private DisplayInfo? _display;
    private BusChoice _outputBus;

    public SettingsViewModel(ProductionService production, ControlHost control, WorkspaceStore store, ILogger<SettingsViewModel> logger)
    {
        _production = production;
        _control = control;
        _store = store;
        _logger = logger;
        _workspace = store.Load();
        _shortcutText = FormatShortcuts(_workspace.Shortcuts);
        _outputBus = BusChoices[0];
        NewShowCommand = new RelayCommand(_ => Guard(NewShow), _ => _production.CanEditInputs);
        OpenShowCommand = new RelayCommand(_ => Guard(OpenShow), _ => _production.CanEditInputs);
        SaveShowCommand = new RelayCommand(_ => Guard(() => SaveShow(_showPath)));
        SaveShowAsCommand = new RelayCommand(_ => Guard(() => SaveShow(null)));
        OpenRecentCommand = new RelayCommand(p => Guard(() => OpenShowFile((string)p!)), _ => _production.CanEditInputs);
        ApplyShortcutsCommand = new RelayCommand(_ => Guard(ApplyShortcuts));
        ResetShortcutsCommand = new RelayCommand(_ =>
        {
            ShortcutText = FormatShortcuts(WorkspaceSettings.DefaultShortcuts());
            Guard(ApplyShortcuts);
        });
        RefreshDisplaysCommand = new RelayCommand(_ => RefreshDisplays());
        OpenOutputCommand = new RelayCommand(_ => Guard(OpenOutput), _ => _display is not null);
        CloseOutputsCommand = new RelayCommand(_ =>
        {
            foreach (ExternalOutputWindow w in OpenOutputs.ToList()) w.Close();
        }, _ => OpenOutputs.Count > 0);
        RefreshDisplays();
    }

    public WorkspaceSettings Workspace => _workspace;

    public ICommand NewShowCommand { get; }
    public ICommand OpenShowCommand { get; }
    public ICommand SaveShowCommand { get; }
    public ICommand SaveShowAsCommand { get; }
    public ICommand OpenRecentCommand { get; }
    public ICommand ApplyShortcutsCommand { get; }
    public ICommand ResetShortcutsCommand { get; }
    public ICommand RefreshDisplaysCommand { get; }
    public ICommand OpenOutputCommand { get; }
    public ICommand CloseOutputsCommand { get; }

    public ObservableCollection<string> RecentShows { get; } = [];
    public ObservableCollection<DisplayInfo> DisplayList { get; } = [];
    public ObservableCollection<ExternalOutputWindow> OpenOutputs { get; } = [];

    public IReadOnlyList<BusChoice> BusChoices { get; } =
    [
        new("Program", VideoBus.Program), new("Preview", VideoBus.Preview), new("Multiview", VideoBus.Multiview),
    ];

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

    /// <summary>Current show file name for the window title (null = unsaved).</summary>
    public string? ShowPath
    {
        get => _showPath;
        private set
        {
            if (SetProperty(ref _showPath, value)) OnPropertyChanged(nameof(ShowName));
        }
    }

    public string ShowName => _showPath is null ? "Untitled show" : Path.GetFileNameWithoutExtension(_showPath);

    public string ShortcutText
    {
        get => _shortcutText;
        set => SetProperty(ref _shortcutText, value ?? "");
    }

    public DisplayInfo? Display
    {
        get => _display;
        set => SetProperty(ref _display, value);
    }

    public BusChoice OutputBus
    {
        get => _outputBus;
        set
        {
            if (value is not null) SetProperty(ref _outputBus, value);
        }
    }

    public string DataFolders =>
        $"Settings: {Path.GetDirectoryName(WorkspaceStore.DefaultPath())}\nLogs: {Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "MixIt", "logs")}\nExtensions: {ExtensionManager.DefaultFolder()}";

    // ---- workspace persistence (window placement, last section)

    public void SaveWorkspace(Func<WorkspaceSettings, WorkspaceSettings> change)
    {
        _workspace = change(_workspace);
        _store.Save(_workspace);
    }

    // ---- shortcuts

    /// <summary>Runs the command bound to a key gesture; false when none is bound.</summary>
    public bool TryRunShortcut(string gesture)
    {
        if (!_workspace.Shortcuts.TryGetValue(gesture, out string? line)) return false;
        MacroStep step;
        try
        {
            step = Macro.Parse("shortcut", line).Steps.Single();
        }
        catch (Exception ex) when (ex is FormatException or InvalidOperationException)
        {
            Error = $"Shortcut {gesture}: {ex.Message}";
            return true;
        }
        _ = RunShortcutAsync(gesture, step);
        return true;
    }

    private async Task RunShortcutAsync(string gesture, MacroStep step)
    {
        CommandResult r = step.Command.Equals("Wait", StringComparison.OrdinalIgnoreCase)
            ? CommandResult.Failure("Wait is for macros, not shortcuts.")
            : await _control.Commands.ExecuteAsync(step.ToRequest(), $"key {gesture}").ConfigureAwait(true);
        Error = r.Ok ? null : r.Error;
    }

    private static string FormatShortcuts(IReadOnlyDictionary<string, string> map)
    {
        var sb = new StringBuilder();
        foreach ((string key, string command) in map.OrderBy(k => k.Key.Length).ThenBy(k => k.Key, StringComparer.OrdinalIgnoreCase))
            sb.Append(key).Append(" = ").AppendLine(command);
        return sb.ToString();
    }

    private void ApplyShortcuts()
    {
        var map = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        string[] lines = _shortcutText.Replace("\r\n", "\n", StringComparison.Ordinal).Split('\n');
        for (int n = 0; n < lines.Length; n++)
        {
            string line = lines[n].Trim();
            if (line.Length == 0 || line.StartsWith('#')) continue;
            int eq = line.IndexOf('=', StringComparison.Ordinal);
            if (eq <= 0) throw new FormatException($"Line {n + 1}: expected Key = Command.");
            string key = line[..eq].Trim();
            string command = line[(eq + 1)..].Trim();
            MacroStep step = Macro.Parse("shortcut", command).Steps.SingleOrDefault()
                ?? throw new FormatException($"Line {n + 1}: no command.");
            if (!_control.Commands.Contains(step.Command)) throw new FormatException($"Line {n + 1}: unknown command '{step.Command}'.");
            if (!map.TryAdd(key, command)) throw new FormatException($"Line {n + 1}: {key} is assigned twice.");
        }
        SaveWorkspace(w => w with { Shortcuts = map });
        Status = $"{map.Count} shortcuts saved.";
    }

    // ---- show files

    private void NewShow()
    {
        if (MessageBox.Show("Start a new, empty show? Unsaved changes to the current show are lost.", "MIX IT",
                MessageBoxButton.OKCancel, MessageBoxImage.Question) != MessageBoxResult.OK) return;
        _production.ImportShow(new ShowDocument { Format = Snapshot().Format }, null);
        ShowPath = null;
        Status = "New show.";
    }

    private ShowDocument Snapshot() => _production.ExportShow(_protector);

    private void OpenShow()
    {
        var dialog = new OpenFileDialog { Filter = "MIX IT show (*.mixit)|*.mixit", Title = "Open show" };
        if (dialog.ShowDialog() == true) OpenShowFile(dialog.FileName);
    }

    /// <summary>Opens a show; problems are listed, nothing is dropped silently.</summary>
    public IReadOnlyList<string> OpenShowFile(string path)
    {
        ShowDocument show;
        try
        {
            show = ShowFile.Load(path);
        }
        catch (Exception ex) when (ex is InvalidDataException or IOException or UnauthorizedAccessException)
        {
            throw new InvalidOperationException($"Cannot open {Path.GetFileName(path)}: {ex.Message}", ex);
        }
        IReadOnlyList<string> warnings = _production.ImportShow(show, _protector);
        ShowPath = path;
        Remember(path);
        Status = warnings.Count == 0 ? $"Opened {Path.GetFileName(path)}." : $"Opened {Path.GetFileName(path)} with {warnings.Count} problem(s).";
        if (warnings.Count > 0)
        {
            _logger.LogWarning("Show {File} opened with problems: {Problems}", path, string.Join("; ", warnings));
            Error = "Opened with problems:\n• " + string.Join("\n• ", warnings);
        }
        return warnings;
    }

    private void SaveShow(string? path)
    {
        if (path is null)
        {
            var dialog = new SaveFileDialog { Filter = "MIX IT show (*.mixit)|*.mixit", Title = "Save show", FileName = ShowName };
            if (dialog.ShowDialog() != true) return;
            path = dialog.FileName;
        }
        SaveShowFile(path);
    }

    public void SaveShowFile(string path)
    {
        ShowFile.Save(Snapshot(), path);
        ShowPath = path;
        Remember(path);
        Status = $"Saved {Path.GetFileName(path)} (stream keys are protected for this Windows user).";
    }

    private void Remember(string path)
    {
        var recent = _workspace.RecentShows.Where(p => !p.Equals(path, StringComparison.OrdinalIgnoreCase)).Prepend(path).Take(8).ToList();
        SaveWorkspace(w => w with { RecentShows = recent });
        RecentShows.Clear();
        foreach (string p in recent) RecentShows.Add(p);
    }

    // ---- external displays

    private void RefreshDisplays()
    {
        DisplayList.Clear();
        try
        {
            foreach (DisplayInfo d in Displays.List()) DisplayList.Add(d);
        }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException)
        {
            Error = $"Displays cannot be listed: {ex.Message}";
        }
        Display = DisplayList.FirstOrDefault(d => !d.IsPrimary) ?? DisplayList.FirstOrDefault();
        RecentShows.Clear();
        foreach (string p in _workspace.RecentShows) RecentShows.Add(p);
    }

    private void OpenOutput()
    {
        DisplayInfo d = _display!;
        if (d.IsPrimary && MessageBox.Show("This display shows MIX IT itself. Cover it with a full-screen output anyway? (Close outputs here or with Alt+Tab.)",
                "MIX IT", MessageBoxButton.OKCancel, MessageBoxImage.Warning) != MessageBoxResult.OK) return;
        var window = new ExternalOutputWindow(_production, _outputBus.Bus, d);
        window.Closed += (_, _) => OpenOutputs.Remove(window);
        OpenOutputs.Add(window);
        window.Show();
        _logger.LogInformation("External output: {Bus} on display {Display}", _outputBus.Bus, d);
        Status = $"{_outputBus.Title} on display {d.Index}.";
    }

    private void Guard(Action action)
    {
        Error = null;
        try
        {
            action();
        }
        catch (Exception ex) when (ex is FormatException or ArgumentException or InvalidOperationException or IOException
                                       or UnauthorizedAccessException or MixItNativeException)
        {
            _logger.LogWarning("Settings operation failed: {Message}", ex.Message);
            Error = ex.Message;
        }
    }
}
