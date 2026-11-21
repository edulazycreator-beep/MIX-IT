using System.IO;
using System.Windows;
using System.Windows.Threading;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using MixIt.Core.Control;
using MixIt.Core.Engine;
using MixIt.Core.Production;
using MixIt.Desktop.Services;
using MixIt.Desktop.ViewModels;
using MixIt.Interop;
using MixIt.Remote;

namespace MixIt.Desktop;

/// <summary>Composition root. Builds services, opens the main window, owns shutdown.</summary>
public partial class App : Application
{
    private ServiceProvider? _services;

    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);

        string logDirectory = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "MixIt", "logs");
        Directory.CreateDirectory(logDirectory);

        var services = new ServiceCollection();
        services.AddLogging(builder => builder.SetMinimumLevel(LogLevel.Information).AddDebug());
        services.AddSingleton(new EngineHostOptions
        {
            MinimumLogLevel = NativeLogLevel.Information,
            LogFilePath = Path.Combine(logDirectory, $"mixit-{DateTime.Now:yyyyMMdd-HHmmss}.jsonl"),
        });
        services.AddSingleton<EngineHost>();
        services.AddSingleton<ProductionService>();
        services.AddSingleton<ProductionViewModel>();
        services.AddSingleton<AudioMixerViewModel>();
        services.AddSingleton<OutputsViewModel>();
        services.AddSingleton<GraphicsViewModel>();
        services.AddSingleton<ReplayViewModel>();
        // M7 control stack (commands run on this UI thread, the owner of ProductionService).
        bool selfTest = e.Args.Length == 2 && e.Args[0] == "--self-test";
        services.AddSingleton<IProductionDispatcher, WpfProductionDispatcher>();
        services.AddSingleton<CommandService>();
        services.AddSingleton<MacroRunner>();
        services.AddSingleton(sp => new AutomationScheduler(sp.GetRequiredService<MacroRunner>(), sp.GetRequiredService<ILogger<AutomationScheduler>>()));
        services.AddSingleton<TallyService>();
        services.AddSingleton<PtzService>();
        services.AddSingleton<ExtensionManager>();
        services.AddSingleton(sp => new ControlSettingsStore(
            selfTest ? Path.Combine(Path.GetTempPath(), "mixit-selftest-control.json") : ControlSettingsStore.DefaultPath(),
            sp.GetRequiredService<ILogger<ControlSettingsStore>>()));
        services.AddSingleton(sp =>
        {
            ProductionService production = sp.GetRequiredService<ProductionService>();
            TallyService tally = sp.GetRequiredService<TallyService>();
            var dispatcher = sp.GetRequiredService<IProductionDispatcher>();
            return new RemoteControlServer(sp.GetRequiredService<CommandService>(), sp.GetRequiredService<MacroRunner>(), tally,
                () => dispatcher.InvokeAsync(() => Task.FromResult(ProductionCommands.Snapshot(production, tally.Current))),
                sp.GetRequiredService<ILogger<RemoteControlServer>>());
        });
        services.AddSingleton<ControlHost>();
        services.AddSingleton<ControlViewModel>();
        services.AddSingleton<DiagnosticsViewModel>();
        services.AddSingleton<MainViewModel>();
        services.AddSingleton<MainWindow>();
        _services = services.BuildServiceProvider();

        // Managed logs join the engine's ordered log stream. Added after the
        // container is built because the provider depends on EngineHost,
        // which itself needs a logger (a DI cycle otherwise).
        var host = _services.GetRequiredService<EngineHost>();
        _services.GetRequiredService<ILoggerFactory>().AddProvider(new NativeEngineLoggerProvider(host));

        ILogger logger = _services.GetRequiredService<ILogger<App>>();
        DispatcherUnhandledException += (_, args) =>
        {
            // A UI fault must not take the production engine down with it.
            logger.LogError(args.Exception, "Unhandled UI exception");
            MessageBox.Show(args.Exception.Message, "MIX IT — interface error", MessageBoxButton.OK, MessageBoxImage.Warning);
            args.Handled = true;
        };

        // Control settings first: the Automation & Control view model reads them.
        ControlHost control = _services.GetRequiredService<ControlHost>();
        Task controlStart = control.StartAsync();
        var window = _services.GetRequiredService<MainWindow>();
        window.Show();
        Task init = _services.GetRequiredService<MainViewModel>().InitializeAsync();

        // CI / support check: drives the real Production Studio view model and
        // monitors, then exits with 0 (pass) or 1 (fail). See SelfTest.cs.
        if (e.Args.Length == 2 && e.Args[0] == "--self-test" && e.Args[1] == "production")
        {
            _ = SelfTest.RunProductionAsync(init, _services.GetRequiredService<ProductionViewModel>(),
                _services.GetRequiredService<AudioMixerViewModel>(), _services.GetRequiredService<OutputsViewModel>(),
                _services.GetRequiredService<GraphicsViewModel>(), _services.GetRequiredService<ReplayViewModel>(),
                control, controlStart, _services.GetRequiredService<ProductionService>(), _services.GetRequiredService<ILogger<App>>(), this);
        }
    }

    protected override void OnExit(ExitEventArgs e)
    {
        // Disposing the provider disposes EngineHost, which shuts the engine
        // down and flushes its log before the process exits.
        if (_services?.GetService<ControlHost>() is { } control)
        {
            // Stops the remote server and macros; never lets a failure block shutdown.
            try
            {
                control.DisposeAsync().AsTask().Wait(TimeSpan.FromSeconds(3));
            }
            catch (AggregateException ex)
            {
                _services.GetRequiredService<ILogger<App>>().LogWarning(ex, "Control shutdown");
            }
        }
        _services?.Dispose();
        base.OnExit(e);
    }
}
