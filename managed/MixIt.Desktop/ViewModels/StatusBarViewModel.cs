using System.Diagnostics;
using System.Globalization;
using System.Windows.Threading;
using MixIt.Core.Control;
using MixIt.Core.Production;
using MixIt.Desktop.Services;
using MixIt.Interop;

namespace MixIt.Desktop.ViewModels;

/// <summary>
/// Production dashboard strip (M8): clock, on-air state, timing health,
/// recordings and streams, remote clients, automation, CPU and memory of MIX IT.
/// Values come from the engine once a second; nothing here is estimated.
/// </summary>
public sealed class StatusBarViewModel : ObservableObject
{
    private readonly ProductionService _production;
    private readonly ControlHost _control;
    private readonly DispatcherTimer _timer;
    private readonly Process _process = Process.GetCurrentProcess();
    private TimeSpan _lastCpu;
    private DateTime _lastSample = DateTime.UtcNow;
    private string _clock = "";
    private string _onAir = "STOPPED";
    private bool _isOnAir;
    private string _timing = "";
    private bool _timingWarning;
    private string _outputs = "";
    private bool _isRecording;
    private bool _isStreaming;
    private bool _outputWarning;
    private string _controlState = "";
    private string _system = "";
    private ulong _lastLate;
    private ulong _lastSkipped;

    public StatusBarViewModel(ProductionService production, ControlHost control)
    {
        _production = production;
        _control = control;
        _lastCpu = _process.TotalProcessorTime;
        _timer = new DispatcherTimer(DispatcherPriority.Background) { Interval = TimeSpan.FromSeconds(1) };
        _timer.Tick += (_, _) => Refresh();
        _timer.Start();
        Refresh();
    }

    public string Clock { get => _clock; private set => SetProperty(ref _clock, value); }
    public string OnAir { get => _onAir; private set => SetProperty(ref _onAir, value); }
    public bool IsOnAir { get => _isOnAir; private set => SetProperty(ref _isOnAir, value); }
    public string Timing { get => _timing; private set => SetProperty(ref _timing, value); }
    /// <summary>Late or skipped ticks in the last second.</summary>
    public bool TimingWarning { get => _timingWarning; private set => SetProperty(ref _timingWarning, value); }
    public string Outputs { get => _outputs; private set => SetProperty(ref _outputs, value); }
    public bool IsRecording { get => _isRecording; private set => SetProperty(ref _isRecording, value); }
    public bool IsStreaming { get => _isStreaming; private set => SetProperty(ref _isStreaming, value); }
    /// <summary>An output is failing or reconnecting.</summary>
    public bool OutputWarning { get => _outputWarning; private set => SetProperty(ref _outputWarning, value); }
    public string Control { get => _controlState; private set => SetProperty(ref _controlState, value); }
    public string Resources { get => _system; private set => SetProperty(ref _system, value); }

    private void Refresh()
    {
        CultureInfo inv = CultureInfo.InvariantCulture;
        Clock = DateTime.Now.ToString("HH:mm:ss", inv);
        SessionStatistics? s = null;
        try
        {
            s = _production.IsRunning ? _production.GetStatistics() : null;
        }
        catch (MixItNativeException)
        {
            // engine restarting: shown as stopped
        }
        IsOnAir = s is not null;
        OnAir = s is null ? "STOPPED" : $"ON AIR  {_production.Format.Name}";
        if (s is { } st)
        {
            ulong late = st.LateTicks - Math.Min(_lastLate, st.LateTicks), skipped = st.SkippedTicks - Math.Min(_lastSkipped, st.SkippedTicks);
            _lastLate = st.LateTicks;
            _lastSkipped = st.SkippedTicks;
            TimingWarning = skipped > 0;
            Timing = skipped > 0 ? $"{skipped} frames skipped in the last second" : late > 0 ? $"{late} late ticks" : $"timing OK · {st.Compositor}";
        }
        else
        {
            _lastLate = _lastSkipped = 0;
            TimingWarning = false;
            Timing = "";
        }

        int rec = 0, live = 0, bad = 0;
        foreach (ProductionOutput o in _production.Outputs)
        {
            OutputState? state = null;
            try
            {
                state = _production.GetOutputStatistics(o.Id)?.State;
            }
            catch (MixItNativeException)
            {
                bad++;
            }
            if (state is OutputState.Running or OutputState.Starting) { if (o.Settings.IsNetwork) live++; else rec++; }
            if (state is OutputState.Failed or OutputState.Reconnecting) bad++;
        }
        IsRecording = rec > 0;
        IsStreaming = live > 0;
        OutputWarning = bad > 0;
        Outputs = (rec, live, bad) switch
        {
            (0, 0, 0) => "",
            _ => string.Join("  ", new[] { rec > 0 ? $"● REC {rec}" : null, live > 0 ? $"● LIVE {live}" : null, bad > 0 ? $"⚠ {bad} output problem(s)" : null }.Where(x => x is not null)),
        };

        var parts = new List<string>();
        if (_control.Remote.IsRunning) parts.Add($"remote {_control.Remote.ClientCount}");
        if (_control.Automation.Armed) parts.Add("automation ARMED");
        if (_control.Macros.RunningCount > 0) parts.Add($"{_control.Macros.RunningCount} macro(s) running");
        Control = string.Join(" · ", parts);

        _process.Refresh();
        DateTime now = DateTime.UtcNow;
        TimeSpan cpu = _process.TotalProcessorTime;
        double percent = (cpu - _lastCpu).TotalMilliseconds / Math.Max(1, (now - _lastSample).TotalMilliseconds) / Environment.ProcessorCount * 100;
        _lastCpu = cpu;
        _lastSample = now;
        Resources = string.Create(inv, $"CPU {percent:0}% · RAM {_process.WorkingSet64 / 1_048_576} MB");
    }
}
