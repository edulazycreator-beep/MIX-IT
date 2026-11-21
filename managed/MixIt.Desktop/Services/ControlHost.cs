using System.Windows;
using System.Windows.Threading;
using Microsoft.Extensions.Logging;
using MixIt.Core.Control;
using MixIt.Core.Production;
using MixIt.Protocol;
using MixIt.Remote;

namespace MixIt.Desktop.Services;

/// <summary>Runs production work on the WPF UI thread (the owner of <see cref="ProductionService"/>).</summary>
public sealed class WpfProductionDispatcher : IProductionDispatcher
{
    public Task<T> InvokeAsync<T>(Func<Task<T>> work)
    {
        ArgumentNullException.ThrowIfNull(work);
        Dispatcher dispatcher = Application.Current?.Dispatcher ?? throw new InvalidOperationException("The application is shutting down.");
        return dispatcher.CheckAccess() ? work() : dispatcher.InvokeAsync(work).Task.Unwrap();
    }
}

/// <summary>
/// Wires the M7 control stack in the desktop app: built-in commands, macros,
/// automation (ticked once a second), tally (refreshed 10 times a second), PTZ,
/// the remote-control server and extensions; loads and saves their settings.
/// </summary>
public sealed class ControlHost : IAsyncDisposable
{
    private readonly ProductionService _production;
    private readonly ControlSettingsStore _store;
    private readonly ILogger<ControlHost> _logger;
    private readonly DispatcherTimer _tallyTimer;
    private readonly DispatcherTimer _automationTimer;
    private bool _wasRunning;

    public ControlHost(ProductionService production, CommandService commands, MacroRunner macros, AutomationScheduler automation,
        TallyService tally, PtzService ptz, ExtensionManager extensions, RemoteControlServer remote, ControlSettingsStore store,
        ILogger<ControlHost> logger)
    {
        _production = production;
        Commands = commands;
        Macros = macros;
        Automation = automation;
        Tally = tally;
        Ptz = ptz;
        Extensions = extensions;
        Remote = remote;
        _store = store;
        _logger = logger;
        ProductionCommands.Register(commands, production);
        macros.Register();
        ptz.Register(commands);
        _tallyTimer = new DispatcherTimer(DispatcherPriority.Background) { Interval = TimeSpan.FromMilliseconds(100) };
        _tallyTimer.Tick += (_, _) => RefreshTally();
        _automationTimer = new DispatcherTimer(DispatcherPriority.Background) { Interval = TimeSpan.FromSeconds(1) };
        _automationTimer.Tick += (_, _) => Automation.Tick();
        production.Changed += (_, _) => OnProductionChanged();
    }

    public CommandService Commands { get; }
    public MacroRunner Macros { get; }
    public AutomationScheduler Automation { get; }
    public TallyService Tally { get; }
    public PtzService Ptz { get; }
    public ExtensionManager Extensions { get; }
    public RemoteControlServer Remote { get; }
    public ControlSettings Settings { get; private set; } = new();

    /// <summary>Last remote-control start error (null = none).</summary>
    public string? RemoteError { get; private set; }

    /// <summary>Loads settings, starts the timers, the remote server (if enabled) and extensions (if enabled).</summary>
    public async Task StartAsync()
    {
        Settings = _store.Load();
        ApplyLists(Settings);
        if (Settings.Remote.Token.Length < 16)
        {
            Settings = Settings with { Remote = Settings.Remote with { Token = RemoteControlServer.NewToken() } };
            Save();
        }
        _tallyTimer.Start();
        _automationTimer.Start();
        if (Settings.ExtensionsEnabled) Extensions.LoadFolder(ExtensionManager.DefaultFolder());
        if (Settings.Remote.Enabled) await StartRemoteAsync().ConfigureAwait(true);
    }

    private void ApplyLists(ControlSettings s)
    {
        var macros = new List<Macro>();
        foreach (MacroDocument d in s.Macros)
        {
            try
            {
                macros.Add(Macro.Parse(d.Name, d.Text, d.StopOnError));
            }
            catch (FormatException ex)
            {
                _logger.LogError("Macro {Name} could not be read: {Message}", d.Name, ex.Message);
            }
        }
        Macros.SetMacros(macros);
        TryApply("automation rules", () => Automation.SetRules(s.Rules));
        TryApply("tally destinations", () => Tally.SetDestinations(s.Tally));
        TryApply("PTZ cameras", () => Ptz.SetCameras(s.Cameras));
    }

    private void TryApply(string what, Action apply)
    {
        try
        {
            apply();
        }
        catch (ArgumentException ex)
        {
            _logger.LogError("Saved {What} are invalid and were not loaded: {Message}", what, ex.Message);
        }
    }

    /// <summary>Changes and saves settings (the caller validated them by applying them first).</summary>
    public void Update(Func<ControlSettings, ControlSettings> change)
    {
        Settings = change(Settings);
        Save();
    }

    public void Save()
    {
        try
        {
            _store.Save(Settings with
            {
                Macros = Macros.Macros.Select(m => new MacroDocument(m.Name, m.ToText(), m.StopOnError)).ToList(),
                Rules = Automation.Rules.ToList(),
                Tally = Tally.Destinations.ToList(),
                Cameras = Ptz.Cameras.ToList(),
            });
        }
        catch (IOException ex)
        {
            _logger.LogError("Control settings could not be saved: {Message}", ex.Message);
        }
    }

    public async Task StartRemoteAsync()
    {
        RemoteError = null;
        try
        {
            await Remote.StartAsync(Settings.Remote).ConfigureAwait(true);
        }
        catch (Exception ex) when (ex is InvalidOperationException or ArgumentException)
        {
            RemoteError = ex.Message;
            _logger.LogError("Remote control did not start: {Message}", ex.Message);
        }
    }

    public Task<ProductionState> SnapshotAsync() =>
        new WpfProductionDispatcher().InvokeAsync(() => Task.FromResult(ProductionCommands.Snapshot(_production, Tally.Current)));

    private void RefreshTally()
    {
        try
        {
            Tally.Refresh();
        }
        catch (Interop.MixItNativeException ex)
        {
            _logger.LogDebug("Tally refresh skipped: {Message}", ex.Message);
        }
    }

    private void OnProductionChanged()
    {
        if (_production.IsRunning == _wasRunning) return;
        _wasRunning = _production.IsRunning;
        if (_wasRunning) Automation.ProductionStarted();
        else Automation.ProductionStopped();
    }

    public async ValueTask DisposeAsync()
    {
        _tallyTimer.Stop();
        _automationTimer.Stop();
        Macros.StopAll();
        await Remote.StopAsync().ConfigureAwait(false);
        Extensions.Dispose();
    }
}
