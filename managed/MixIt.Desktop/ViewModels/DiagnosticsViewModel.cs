using System.Collections.ObjectModel;
using Microsoft.Extensions.Logging;
using MixIt.Core.Engine;
using MixIt.Core.Hardware;
using MixIt.Interop;

namespace MixIt.Desktop.ViewModels;

/// <summary>Row in the diagnostics key/value list.</summary>
public sealed record InfoRow(string Label, string Value);

/// <summary>Row in the engine log view.</summary>
public sealed record LogRow(string Time, string Level, string Category, string Message, bool IsProblem);

/// <summary>
/// Hardware Diagnostics screen. All engine calls run off the UI thread;
/// results are applied on the UI thread after await.
/// </summary>
public sealed class DiagnosticsViewModel : ObservableObject
{
    private const int MaxLogRows = 1000;

    private readonly EngineHost _host;
    private readonly ILogger<DiagnosticsViewModel> _logger;
    private string _summary = "Engine not started";
    private string? _error;
    private bool _logPumpStarted;

    public DiagnosticsViewModel(EngineHost host, ILogger<DiagnosticsViewModel> logger)
    {
        _host = host;
        _logger = logger;
        DetectCommand = new AsyncCommand(DetectAsync, () => _host.State == EngineHostState.Running);
        RestartCommand = new AsyncCommand(RestartAsync);
    }

    public ObservableCollection<InfoRow> SystemRows { get; } = [];
    public ObservableCollection<InfoRow> Gpus { get; } = [];
    public ObservableCollection<LogRow> Log { get; } = [];
    public AsyncCommand DetectCommand { get; }
    public AsyncCommand RestartCommand { get; }

    public string Summary
    {
        get => _summary;
        private set => SetProperty(ref _summary, value);
    }

    /// <summary>Last engine error to show the operator, or null.</summary>
    public string? Error
    {
        get => _error;
        private set => SetProperty(ref _error, value);
    }

    public async Task StartEngineAsync()
    {
        StartLogPump();
        try
        {
            await Task.Run(_host.Start);
            Error = null;
            await DetectAsync();
        }
        catch (MixItNativeException ex)
        {
            Error = ex.Message;
            Summary = "Engine failed to start";
        }
        DetectCommand.RaiseCanExecuteChanged();
    }

    private async Task RestartAsync()
    {
        await Task.Run(_host.Stop);
        await StartEngineAsync();
    }

    private async Task DetectAsync()
    {
        try
        {
            SystemCapabilities caps = await Task.Run(_host.DetectCapabilities);
            Apply(caps);
            Summary = $"Engine {NativeEngine.LibraryApiVersion} — {EngineHost.NativeBuildInfo}";
        }
        catch (Exception ex) when (ex is MixItNativeException or InvalidOperationException)
        {
            _logger.LogWarning(ex, "Capability detection failed");
            Error = ex.Message;
        }
    }

    private void Apply(SystemCapabilities c)
    {
        SystemRows.Clear();
        SystemRows.Add(new("Operating system", Or(c.OperatingSystem)));
        SystemRows.Add(new("CPU", Or(c.CpuBrand)));
        SystemRows.Add(new("Cores", $"{c.LogicalProcessorCount} logical / {(c.PhysicalCoreCount > 0 ? c.PhysicalCoreCount.ToString() : "unknown")} physical"));
        SystemRows.Add(new("CPU features", CapabilityFormatting.FormatCpuFeatures(c.CpuFeatures)));
        SystemRows.Add(new("Memory", $"{CapabilityFormatting.FormatBytes(c.AvailablePhysicalMemoryBytes)} available of {CapabilityFormatting.FormatBytes(c.TotalPhysicalMemoryBytes)}"));
        SystemRows.Add(new("GPU", CapabilityFormatting.DescribeGpuAvailability(c)));

        Gpus.Clear();
        foreach (GpuAdapterInfo g in c.GpuAdapters)
        {
            string kind = g.IsSoftware ? " · software" : string.Empty;
            Gpus.Add(new(g.Description, $"{g.VendorName} · VRAM {CapabilityFormatting.FormatBytes(g.DedicatedVideoMemoryBytes)}{kind}"));
        }
    }

    // Single consumer of the engine log stream. Runs on the UI context, so
    // collection updates happen on the UI thread; the stream itself is
    // bounded, so a busy UI never stalls the engine.
    private async void StartLogPump()
    {
        if (_logPumpStarted) return;
        _logPumpStarted = true;
        try
        {
            await foreach (NativeLogEntry e in _host.LogStream.Reader.ReadAllAsync())
            {
                Log.Add(new LogRow(e.Timestamp.ToLocalTime().ToString("HH:mm:ss.fff"), e.Level.ToString(), e.Category,
                    e.Message, e.Level >= NativeLogLevel.Warning));
                if (Log.Count > MaxLogRows) Log.RemoveAt(0);
            }
        }
        catch (Exception ex)
        {
            _logger.LogError(ex, "Log view pump stopped");
        }
    }

    private static string Or(string s) => string.IsNullOrWhiteSpace(s) ? "unknown" : s;
}
