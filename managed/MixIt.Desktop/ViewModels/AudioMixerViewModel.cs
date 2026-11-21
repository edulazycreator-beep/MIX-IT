using System.Collections.ObjectModel;
using System.Globalization;
using System.Windows.Input;
using System.Windows.Threading;
using Microsoft.Extensions.Logging;
using MixIt.Core.Production;
using MixIt.Interop;

namespace MixIt.Desktop.ViewModels;

/// <summary>
/// Audio Mixer section: one strip per input (fader, mute, solo, audio-follows-
/// video, bus sends, balance, meters) plus the master and aux buses. Detailed
/// processing (trim, delay, high-pass, EQ, gate, compressor) is edited for the
/// selected strip. Every change is sent to the engine immediately through
/// <see cref="ProductionService"/>; meters are polled at 20 Hz while visible.
/// </summary>
public sealed class AudioMixerViewModel : ObservableObject
{
    private static readonly TimeSpan MeterInterval = TimeSpan.FromMilliseconds(50);

    private readonly ProductionService _production;
    private readonly ILogger<AudioMixerViewModel> _logger;
    private readonly DispatcherTimer _timer;
    private StripViewModel? _selected;
    private AudioBus _monitorSource = AudioBus.Master;
    private string? _error;
    private bool _active;
    private DeviceChoice _selectedOutput = DeviceChoice.Off;
    private AudioDevice? _selectedInputDevice;
    private string _outputStatus = "";
    private Vst3Plugin? _selectedPlugin;
    private string _pluginStatus = "Not scanned yet";

    public AudioMixerViewModel(ProductionService production, ILogger<AudioMixerViewModel> logger)
    {
        _production = production;
        _logger = logger;
        _timer = new DispatcherTimer(DispatcherPriority.Background) { Interval = MeterInterval };
        _timer.Tick += (_, _) => UpdateMeters();
        _production.Changed += (_, _) => Sync();
        foreach (AudioBus bus in new[] { AudioBus.Master, AudioBus.AuxA, AudioBus.AuxB, AudioBus.AuxC, AudioBus.AuxD })
        {
            Buses.Add(new BusViewModel(bus, this));
        }
        MonitorBus = new BusViewModel(AudioBus.Monitor, this);
        OutputChoices.Add(DeviceChoice.Off);
        RefreshDevicesCommand = new RelayCommand(_ => Guard(RefreshDevices), _ => _production.EngineAvailable);
        AddAudioInputCommand = new RelayCommand(_ => Guard(() => _production.AddAudioCapture(_selectedInputDevice!)),
            _ => _selectedInputDevice is not null && _production.CanEditInputs && _production.EngineAvailable);
        ScanPluginsCommand = new AsyncCommand(ScanPluginsAsync, () => _production.EngineAvailable);
        Sync();
    }

    /// <summary>Monitor/headphone output choices: Off, the default device, then each device.</summary>
    public ObservableCollection<DeviceChoice> OutputChoices { get; } = [];
    public ObservableCollection<AudioDevice> InputDevices { get; } = [];
    public ICommand RefreshDevicesCommand { get; }
    public ICommand AddAudioInputCommand { get; }

    public DeviceChoice SelectedOutput
    {
        get => _selectedOutput;
        set
        {
            if (value is null || !SetProperty(ref _selectedOutput, value)) return;
            Guard(() => _production.SetAudioOutput(value.DeviceId));
        }
    }

    public AudioDevice? SelectedInputDevice
    {
        get => _selectedInputDevice;
        set => SetProperty(ref _selectedInputDevice, value);
    }

    /// <summary>Monitor output state for the operator (device, buffer, problems).</summary>
    public string OutputStatus
    {
        get => _outputStatus;
        private set => SetProperty(ref _outputStatus, value);
    }

    /// <summary>Lists audio devices (call when the engine is up or on demand).</summary>
    public void RefreshDevices()
    {
        string? current = _production.AudioOutputDeviceId;
        OutputChoices.Clear();
        OutputChoices.Add(DeviceChoice.Off);
        OutputChoices.Add(new DeviceChoice("Default output device", ""));
        foreach (AudioDevice d in _production.GetAudioDevices(AudioDeviceDirection.Render))
        {
            OutputChoices.Add(new DeviceChoice(d.Name, d.Id));
        }
        _selectedOutput = OutputChoices.FirstOrDefault(c => c.DeviceId == current) ?? DeviceChoice.Off;
        OnPropertyChanged(nameof(SelectedOutput));
        InputDevices.Clear();
        foreach (AudioDevice d in _production.GetAudioDevices(AudioDeviceDirection.Capture)) InputDevices.Add(d);
        SelectedInputDevice = InputDevices.FirstOrDefault(d => d.IsDefault) ?? InputDevices.FirstOrDefault();
        if (OutputChoices.Count == 2 && InputDevices.Count == 0) OutputStatus = "No audio devices found.";
    }

    /// <summary>VST3 effects found by the last scan (default plug-in folders).</summary>
    public ObservableCollection<Vst3Plugin> AvailablePlugins { get; } = [];
    public AsyncCommand ScanPluginsCommand { get; }

    /// <summary>Plug-in chosen for "Load" in an insert slot.</summary>
    public Vst3Plugin? SelectedPlugin
    {
        get => _selectedPlugin;
        set => SetProperty(ref _selectedPlugin, value);
    }

    public string PluginStatus
    {
        get => _pluginStatus;
        private set => SetProperty(ref _pluginStatus, value);
    }

    private async Task ScanPluginsAsync()
    {
        PluginStatus = "Scanning plug-in folders…";
        try
        {
            IReadOnlyList<Vst3Plugin> found = await _production.ScanPluginsAsync();
            AvailablePlugins.Clear();
            foreach (Vst3Plugin p in found.OrderBy(p => p.Name, StringComparer.CurrentCultureIgnoreCase)) AvailablePlugins.Add(p);
            SelectedPlugin = AvailablePlugins.FirstOrDefault();
            PluginStatus = found.Count == 0 ? "No VST3 effects found" : $"{found.Count} VST3 effect(s) found";
        }
        catch (Exception ex) when (ex is MixItNativeException or InvalidOperationException)
        {
            _logger.LogWarning(ex, "Plug-in scan failed");
            PluginStatus = "Scan failed: " + ex.Message;
        }
    }

    internal Task ScanPluginsAsyncForTest() => ScanPluginsAsync();

    /// <summary>Async variant of <see cref="Guard"/> for slow engine operations.</summary>
    internal async Task GuardAsync(Func<Task> action)
    {
        Error = null;
        try
        {
            await action();
        }
        catch (Exception ex) when (ex is MixItNativeException or InvalidOperationException or ArgumentException)
        {
            _logger.LogWarning(ex, "Audio mixer operation failed");
            Error = ex.Message;
        }
    }

    public ObservableCollection<StripViewModel> Strips { get; } = [];
    public ObservableCollection<BusViewModel> Buses { get; } = [];
    public BusViewModel MonitorBus { get; }
    public IReadOnlyList<AudioBus> MonitorSources { get; } = [AudioBus.Master, AudioBus.AuxA, AudioBus.AuxB, AudioBus.AuxC, AudioBus.AuxD];

    public StripViewModel? Selected
    {
        get => _selected;
        set
        {
            if (!SetProperty(ref _selected, value)) return;
            OnPropertyChanged(nameof(HasSelection));
            if (value is not null) Guard(value.LoadInserts);
        }
    }

    public bool HasSelection => _selected is not null;

    public AudioBus MonitorSource
    {
        get => _monitorSource;
        set
        {
            if (!SetProperty(ref _monitorSource, value)) return;
            Guard(() => _production.SetMonitorSource(value));
        }
    }

    public string? Error
    {
        get => _error;
        private set => SetProperty(ref _error, value);
    }

    public bool HasInputs => Strips.Count > 0;

    /// <summary>Meters poll only while the section is visible.</summary>
    public bool IsActive
    {
        get => _active;
        set
        {
            if (!SetProperty(ref _active, value)) return;
            if (value && OutputChoices.Count == 1) Guard(RefreshDevices);  // first visit: list devices
            UpdateTimer();
        }
    }

    internal ProductionService Production => _production;

    internal void Guard(Action action)
    {
        Error = null;
        try
        {
            action();
        }
        catch (Exception ex) when (ex is MixItNativeException or InvalidOperationException or ArgumentException)
        {
            _logger.LogWarning(ex, "Audio mixer operation failed");
            Error = ex.Message;
        }
    }

    private void Sync()
    {
        uint? selectedId = _selected?.Id;
        Strips.Clear();
        if (_production.EngineAvailable)
        {
            foreach (ProductionInput input in _production.Inputs)
            {
                if (input.Kind == ProductionInputKind.Graphics) continue;  // no audio
                try
                {
                    Strips.Add(new StripViewModel(input, _production.GetAudioStrip(input.Id), this));
                }
                catch (Exception ex) when (ex is MixItNativeException or InvalidOperationException)
                {
                    _logger.LogWarning(ex, "Cannot read the audio strip of input {Id}", input.Id);
                }
            }
            foreach (BusViewModel bus in Buses) bus.Load();
            _monitorSource = _production.MonitorSource;
            OnPropertyChanged(nameof(MonitorSource));
        }
        Selected = Strips.FirstOrDefault(s => s.Id == selectedId) ?? Strips.FirstOrDefault();
        OnPropertyChanged(nameof(HasInputs));
        UpdateTimer();
    }

    private void UpdateOutputStatus()
    {
        AudioOutputStatistics? o = _production.GetAudioOutputStatistics();
        OutputStatus = o is null || o.State == AudioOutputState.Off
            ? "Monitor output off"
            : o.State == AudioOutputState.Retrying
                ? $"Monitor output: device unavailable, retrying ({o.DeviceErrors} errors)"
                : $"Monitor output: {o.DeviceName} · {o.BufferedMilliseconds} ms buffered · {o.Underruns} dropouts";
    }

    private void UpdateTimer()
    {
        bool run = _active && _production.IsRunning;
        if (_active && _production.EngineAvailable) UpdateOutputStatus();
        if (run && !_timer.IsEnabled) _timer.Start();
        if (!run && _timer.IsEnabled) _timer.Stop();
        if (!run)
        {
            foreach (StripViewModel s in Strips) s.ClearMeters();
            foreach (BusViewModel b in Buses) b.ClearMeters();
            MonitorBus.ClearMeters();
        }
    }

    private void UpdateMeters()
    {
        UpdateOutputStatus();
        if (!_production.IsRunning) return;
        foreach (StripViewModel s in Strips) s.ShowMeters(_production.GetInputMeter(s.Id));
        foreach (BusViewModel b in Buses) b.ShowMeters(_production.GetBusMeter(b.Bus));
        MonitorBus.ShowMeters(_production.GetBusMeter(AudioBus.Monitor));
    }
}

/// <summary>An output device choice (null id = off, "" = default device).</summary>
public sealed record DeviceChoice(string Name, string? DeviceId)
{
    public static DeviceChoice Off { get; } = new("Off", null);

    public override string ToString() => Name;
}

/// <summary>Shared meter presentation: two peak bars (dBFS) and a readout.</summary>
public abstract class MeteredViewModel : ObservableObject
{
    /// <summary>Bottom of the meter scale (dBFS).</summary>
    public const double MeterFloor = -60;

    private double _peakLeft = MeterFloor;
    private double _peakRight = MeterFloor;
    private bool _clip;

    public double PeakLeft
    {
        get => _peakLeft;
        private set => SetProperty(ref _peakLeft, value);
    }

    public double PeakRight
    {
        get => _peakRight;
        private set => SetProperty(ref _peakRight, value);
    }

    /// <summary>True while a channel is at or above -0.1 dBFS.</summary>
    public bool Clip
    {
        get => _clip;
        private set => SetProperty(ref _clip, value);
    }

    protected void SetPeaks(AudioMeterReading? m)
    {
        double l = m is { Channels: > 0 } ? m.PeakDb[0] : MeterFloor;
        double r = m is { Channels: > 1 } ? m.PeakDb[1] : l;
        PeakLeft = Math.Max(MeterFloor, l);
        PeakRight = Math.Max(MeterFloor, r);
        Clip = l >= -0.1 || r >= -0.1;
    }

    protected static string Db(float v) =>
        v <= AudioMeterReading.Silence + 1 ? "-∞" : v.ToString("0.0", CultureInfo.InvariantCulture);
}

/// <summary>One input's channel strip.</summary>
public sealed class StripViewModel : MeteredViewModel
{
    private readonly AudioMixerViewModel _owner;
    private AudioStripSettings _s;
    private string _reduction = "";

    internal StripViewModel(ProductionInput input, AudioStripSettings settings, AudioMixerViewModel owner)
    {
        Id = input.Id;
        Name = input.Name;
        _s = settings;
        _owner = owner;
        SelectCommand = new RelayCommand(_ => _owner.Selected = this);
        for (int slot = 0; slot < NativeSession.AudioInsertSlots; slot++) Inserts.Add(new InsertSlotViewModel(this, slot, owner));
    }

    /// <summary>VST3 insert slots (before the strip processing).</summary>
    public ObservableCollection<InsertSlotViewModel> Inserts { get; } = [];

    /// <summary>Reads the insert slots from the engine (when the strip is shown in detail).</summary>
    public void LoadInserts()
    {
        IReadOnlyList<AudioInsert?> current = _owner.Production.GetInserts(Id);
        for (int slot = 0; slot < Inserts.Count; slot++) Inserts[slot].Show(current[slot]);
    }

    public uint Id { get; }
    public string Name { get; }
    public ICommand SelectCommand { get; }

    /// <summary>Fader in dB; the bottom of the slider (-60) means off.</summary>
    public double FaderDb
    {
        get => _s.FaderDb <= AudioStripSettings.FaderOffDb ? MeterFloor : _s.FaderDb;
        set => Apply(_s with { FaderDb = value <= MeterFloor + 0.01 ? AudioStripSettings.FaderOffDb : (float)value });
    }

    public string FaderText => _s.FaderDb <= AudioStripSettings.FaderOffDb ? "off" : _s.FaderDb.ToString("+0.0;-0.0;0.0", CultureInfo.InvariantCulture);

    public bool Mute { get => _s.Mute; set => Apply(_s with { Mute = value }); }
    public bool Solo { get => _s.Solo; set => Apply(_s with { Solo = value }); }
    public bool AudioFollowsVideo { get => _s.AudioFollowsVideo; set => Apply(_s with { AudioFollowsVideo = value }); }
    public bool ToMaster { get => _s.SendsTo(AudioBus.Master); set => SetBus(AudioBus.Master, value); }
    public bool ToA { get => _s.SendsTo(AudioBus.AuxA); set => SetBus(AudioBus.AuxA, value); }
    public bool ToB { get => _s.SendsTo(AudioBus.AuxB); set => SetBus(AudioBus.AuxB, value); }
    public bool ToC { get => _s.SendsTo(AudioBus.AuxC); set => SetBus(AudioBus.AuxC, value); }
    public bool ToD { get => _s.SendsTo(AudioBus.AuxD); set => SetBus(AudioBus.AuxD, value); }
    public double Balance { get => _s.Balance; set => Apply(_s with { Balance = (float)Math.Clamp(value, -1, 1) }); }
    public double TrimDb { get => _s.TrimDb; set => Apply(_s with { TrimDb = (float)value }); }
    public double DelayMs { get => _s.DelayMs; set => Apply(_s with { DelayMs = (float)Math.Round(value) }); }
    public bool HighPass { get => _s.HighPass; set => Apply(_s with { HighPass = value }); }
    public double HighPassHz { get => _s.HighPassHz; set => Apply(_s with { HighPassHz = (float)Math.Round(value) }); }

    // EQ: four fixed-shape bands; a band is active while its gain is not 0 dB.
    public double EqLowDb { get => _s.Eq[0].GainDb; set => SetEq(0, value); }
    public double EqLowMidDb { get => _s.Eq[1].GainDb; set => SetEq(1, value); }
    public double EqHighMidDb { get => _s.Eq[2].GainDb; set => SetEq(2, value); }
    public double EqHighDb { get => _s.Eq[3].GainDb; set => SetEq(3, value); }

    public bool GateEnabled { get => _s.Gate.Enabled; set => Apply(_s with { Gate = _s.Gate with { Enabled = value } }); }
    public double GateThresholdDb { get => _s.Gate.ThresholdDb; set => Apply(_s with { Gate = _s.Gate with { ThresholdDb = (float)value } }); }
    public bool CompressorEnabled { get => _s.Compressor.Enabled; set => Apply(_s with { Compressor = _s.Compressor with { Enabled = value } }); }
    public double CompressorThresholdDb { get => _s.Compressor.ThresholdDb; set => Apply(_s with { Compressor = _s.Compressor with { ThresholdDb = (float)value } }); }
    public double CompressorRatio { get => _s.Compressor.Ratio; set => Apply(_s with { Compressor = _s.Compressor with { Ratio = (float)Math.Round(value, 1) } }); }
    public double CompressorMakeupDb { get => _s.Compressor.MakeupDb; set => Apply(_s with { Compressor = _s.Compressor with { MakeupDb = (float)value } }); }

    /// <summary>Gain reduction readout (compressor / gate) while they act.</summary>
    public string Reduction
    {
        get => _reduction;
        private set => SetProperty(ref _reduction, value);
    }

    internal void ShowMeters(AudioMeterReading? m)
    {
        SetPeaks(m);
        if (m is null)
        {
            Reduction = "";
            return;
        }
        string comp = m.GainReductionDb < -0.5f ? $"comp {Db(m.GainReductionDb)}" : "";
        string gate = m.GateReductionDb < -3f ? "gate closed" : "";
        Reduction = string.Join(" · ", new[] { comp, gate }.Where(t => t.Length > 0));
    }

    internal void ClearMeters() => ShowMeters(null);

    private void SetBus(AudioBus bus, bool on)
    {
        uint bit = 1u << (int)bus;
        Apply(_s with { BusMask = on ? _s.BusMask | bit : _s.BusMask & ~bit });
    }

    private void SetEq(int band, double gain)
    {
        var bands = _s.Eq.ToArray();
        float g = (float)Math.Round(gain, 1);
        bands[band] = bands[band] with { GainDb = g, Enabled = g != 0f };
        Apply(_s with { Eq = bands });
    }

    private void Apply(AudioStripSettings next)
    {
        if (next == _s) return;
        _owner.Guard(() =>
        {
            _owner.Production.SetAudioStrip(Id, next);
            _s = next;
        });
        // Refresh every binding of the strip: accepted values show their new
        // text, rejected ones snap back to what the engine has.
        OnPropertyChanged(string.Empty);
    }
}

/// <summary>Master / aux bus strip, or the read-only monitor bus.</summary>
public sealed class BusViewModel : MeteredViewModel
{
    private readonly AudioMixerViewModel _owner;
    private AudioBusSettings _s = new();
    private string _loudness = "";

    internal BusViewModel(AudioBus bus, AudioMixerViewModel owner)
    {
        Bus = bus;
        _owner = owner;
        ResetLoudnessCommand = new RelayCommand(_ => _owner.Guard(() => _owner.Production.ResetLoudness(Bus)),
            _ => Bus != AudioBus.Monitor && _owner.Production.EngineAvailable);
    }

    public AudioBus Bus { get; }
    public string Name => Bus switch
    {
        AudioBus.Master => "MASTER",
        AudioBus.Monitor => "MONITOR",
        _ => "AUX " + Bus.ToString()[^1],
    };
    public bool IsMaster => Bus == AudioBus.Master;
    public ICommand ResetLoudnessCommand { get; }

    public double FaderDb
    {
        get => _s.FaderDb <= AudioStripSettings.FaderOffDb ? MeterFloor : _s.FaderDb;
        set => Apply(_s with { FaderDb = value <= MeterFloor + 0.01 ? AudioStripSettings.FaderOffDb : (float)value });
    }

    public string FaderText => _s.FaderDb <= AudioStripSettings.FaderOffDb ? "off" : _s.FaderDb.ToString("+0.0;-0.0;0.0", CultureInfo.InvariantCulture);
    public bool Mute { get => _s.Mute; set => Apply(_s with { Mute = value }); }
    public bool Limiter { get => _s.Limiter; set => Apply(_s with { Limiter = value }); }

    /// <summary>EBU R128 readout: momentary / short-term / integrated LUFS.</summary>
    public string Loudness
    {
        get => _loudness;
        private set => SetProperty(ref _loudness, value);
    }

    internal void Load()
    {
        if (Bus == AudioBus.Monitor) return;
        _owner.Guard(() => _s = _owner.Production.GetAudioBus(Bus));
        OnPropertyChanged(string.Empty);
    }

    internal void ShowMeters(AudioMeterReading? m)
    {
        SetPeaks(m);
        Loudness = m is null || Bus == AudioBus.Monitor
            ? ""
            : $"M {Db(m.MomentaryLufs)}  S {Db(m.ShortTermLufs)}  I {Db(m.IntegratedLufs)} LUFS";
    }

    internal void ClearMeters() => ShowMeters(null);

    private void Apply(AudioBusSettings next)
    {
        if (Bus == AudioBus.Monitor || next == _s) return;
        _owner.Guard(() =>
        {
            _owner.Production.SetAudioBus(Bus, next);
            _s = next;
        });
        OnPropertyChanged(string.Empty);
    }
}

/// <summary>One plug-in insert slot of a strip, with a generic parameter editor.</summary>
public sealed class InsertSlotViewModel : ObservableObject
{
    private readonly StripViewModel _strip;
    private readonly AudioMixerViewModel _owner;
    private AudioInsert? _insert;

    internal InsertSlotViewModel(StripViewModel strip, int slot, AudioMixerViewModel owner)
    {
        _strip = strip;
        _owner = owner;
        Slot = slot;
        LoadCommand = new AsyncCommand(LoadAsync, () => _owner.SelectedPlugin is not null && _owner.Production.EngineAvailable);
        RemoveCommand = new RelayCommand(_ => _owner.Guard(() =>
        {
            _owner.Production.RemoveInsert(_strip.Id, Slot);
            Show(null);
        }), _ => _insert is not null);
    }

    public int Slot { get; }
    public string SlotName => $"Insert {Slot + 1}";
    public bool IsLoaded => _insert is not null;
    public string Title => _insert is null ? "empty" : _insert.Vendor.Length == 0 ? _insert.Name : $"{_insert.Name} · {_insert.Vendor}";

    /// <summary>Problems the operator should know about (latency not compensated, failed blocks).</summary>
    public string Notes => _insert is null
        ? ""
        : string.Join("  ", new[]
        {
            _insert.LatencySamples > 0 ? $"latency {_insert.LatencySamples} samples (not compensated)" : "",
            _insert.ProcessErrors > 0 ? $"{_insert.ProcessErrors} failed blocks (passed through)" : "",
        }.Where(t => t.Length > 0));

    public bool Bypass
    {
        get => _insert?.Bypass ?? false;
        set
        {
            if (_insert is null || _insert.Bypass == value) return;
            _owner.Guard(() =>
            {
                _owner.Production.SetInsertBypass(_strip.Id, Slot, value);
                _insert = _insert with { Bypass = value };
            });
            OnPropertyChanged();
        }
    }

    public ObservableCollection<PluginParameterViewModel> Parameters { get; } = [];
    public AsyncCommand LoadCommand { get; }
    public ICommand RemoveCommand { get; }

    internal void Show(AudioInsert? insert)
    {
        _insert = insert;
        Parameters.Clear();
        if (insert is not null)
        {
            IReadOnlyList<AudioInsertParameter> list = _owner.Production.GetInsertParameters(_strip.Id, Slot);
            for (int i = 0; i < list.Count; i++)
            {
                if (list[i].IsBypass) continue;  // the slot's Bypass toggle covers it
                Parameters.Add(new PluginParameterViewModel(this, i, list[i]));
            }
        }
        OnPropertyChanged(nameof(IsLoaded));
        OnPropertyChanged(nameof(Title));
        OnPropertyChanged(nameof(Notes));
        OnPropertyChanged(nameof(Bypass));
    }

    internal AudioInsertParameter SetParameter(int index, uint id, double value)
    {
        _owner.Production.SetInsertParameter(_strip.Id, Slot, id, value);
        return _owner.Production.GetInsertParameter(_strip.Id, Slot, index);
    }

    internal void Guard(Action action) => _owner.Guard(action);

    private Task LoadAsync() => _owner.GuardAsync(async () =>
    {
        Vst3Plugin plugin = _owner.SelectedPlugin!;
        await _owner.Production.SetInsertAsync(_strip.Id, Slot, plugin);
        // SetInsertAsync raises Changed, which rebuilds the strips; refresh this one if it survived.
        Show(_owner.Production.GetInserts(_strip.Id)[Slot]);
    });
}

/// <summary>A plug-in parameter as a 0..1 slider with the plug-in's own value text.</summary>
public sealed class PluginParameterViewModel : ObservableObject
{
    private readonly InsertSlotViewModel _slot;
    private readonly int _index;
    private AudioInsertParameter _p;

    internal PluginParameterViewModel(InsertSlotViewModel slot, int index, AudioInsertParameter parameter)
    {
        _slot = slot;
        _index = index;
        _p = parameter;
    }

    public string Title => _p.Units.Length == 0 ? _p.Title : $"{_p.Title} ({_p.Units})";
    public bool IsEditable => !_p.ReadOnly;
    public bool IsStepped => _p.StepCount > 0;
    public double Step => _p.StepCount > 0 ? 1.0 / _p.StepCount : 0.01;
    public string Display => _p.Display.Length > 0 ? _p.Display : _p.Value.ToString("0.00", CultureInfo.InvariantCulture);

    public double Value
    {
        get => _p.Value;
        set
        {
            double v = Math.Clamp(value, 0, 1);
            if (Math.Abs(v - _p.Value) < 1e-9 || _p.ReadOnly) return;
            _slot.Guard(() => _p = _slot.SetParameter(_index, _p.Id, v));
            OnPropertyChanged();
            OnPropertyChanged(nameof(Display));
        }
    }
}
