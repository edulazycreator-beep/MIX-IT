using System.IO;
using System.Windows;
using System.Windows.Threading;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using MixIt.Core.Engine;
using MixIt.Desktop.ViewModels;
using MixIt.Interop;

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

        var window = _services.GetRequiredService<MainWindow>();
        window.Show();
        _ = _services.GetRequiredService<MainViewModel>().InitializeAsync();
    }

    protected override void OnExit(ExitEventArgs e)
    {
        // Disposing the provider disposes EngineHost, which shuts the engine
        // down and flushes its log before the process exits.
        _services?.Dispose();
        base.OnExit(e);
    }
}
