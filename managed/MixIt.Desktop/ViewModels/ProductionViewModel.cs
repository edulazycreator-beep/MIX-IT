using System.Collections.ObjectModel;
using System.Windows;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using Microsoft.Extensions.Logging;
using Microsoft.Win32;
using MixIt.Core.Engine;
using MixIt.Core.Production;
using MixIt.Interop;

namespace MixIt.Desktop.ViewModels;

/// <summary>One input tile with its tally state.</summary>
public sealed class InputTileViewModel(ProductionInput input) : ObservableObject
{
    private bool _onProgram;
    private bool _onPreview;

    public uint Id { get; } = input.Id;
    public string Name { get; } = input.Name;
    public string Detail { get; } = input.Detail;
    public string Number { get; init; } = "";

    /// <summary>Click: put this input on preview.</summary>
    public ICommand? SelectPreview { get; init; }

    /// <summary>Double-click: put this input straight on program.</summary>
    public ICommand? TakeToProgram { get; init; }

    public ICommand? Remove { get; init; }

    public bool OnProgram
    {
        get => _onProgram;
        set => SetProperty(ref _onProgram, value);
    }

    public bool OnPreview
    {
        get => _onPreview;
        set => SetProperty(ref _onPreview, value);
    }
}

/// <summary>A bus monitor: a WriteableBitmap refreshed from the engine.</summary>
public sealed class BusMonitor : ObservableObject
{
    private WriteableBitmap? _bitmap;
    private ulong _lastTick = ulong.MaxValue;
    private string _source = "—";

    public BusMonitor(VideoBus bus) => Bus = bus;

    public VideoBus Bus { get; }

    public WriteableBitmap? Bitmap
    {
        get => _bitmap;
        private set => SetProperty(ref _bitmap, value);
    }

    /// <summary>Name of the input currently on this bus.</summary>
    public string Source
    {
        get => _source;
        set => SetProperty(ref _source, value);
    }

    /// <summary>Distinct engine frames shown so far (diagnostics and self-test).</summary>
    public long FramesShown { get; private set; }

    /// <summary>Copies the newest bus frame straight into the bitmap's back buffer.</summary>
    public void Refresh(ProductionService production, int width, int height)
    {
        if (_bitmap is null || _bitmap.PixelWidth != width || _bitmap.PixelHeight != height)
        {
            // Bgr32: the engine's frames are opaque BGRA, alpha is ignored.
            Bitmap = new WriteableBitmap(width, height, 96, 96, PixelFormats.Bgr32, null);
            _lastTick = ulong.MaxValue;
        }
        WriteableBitmap bitmap = _bitmap!;
        bitmap.Lock();
        try
        {
            int length = bitmap.BackBufferStride * height;
            ProgramFrameInfo? info = production.TryCopyBusFrame(Bus, bitmap.BackBuffer, length, bitmap.BackBufferStride);
            if (info is { } f && f.TickIndex != _lastTick && f.Width == width && f.Height == height)
            {
                _lastTick = f.TickIndex;
                bitmap.AddDirtyRect(new Int32Rect(0, 0, width, height));
                FramesShown++;
            }
        }
        finally
        {
            bitmap.Unlock();
        }
    }

    public void Clear()
    {
        Bitmap = null;
        Source = "—";
    }
}

/// <summary>
/// Production Studio: preview/program monitors, input tiles with tally,
/// CUT / AUTO transitions and a picture-in-picture overlay. Engine work goes
/// through <see cref="ProductionService"/>; this class only binds and polls.
/// </summary>
public sealed class ProductionViewModel : ObservableObject
{
    private static readonly TimeSpan MonitorInterval = TimeSpan.FromMilliseconds(40);  // 25 Hz monitors

    private readonly ProductionService _production;
    private readonly EngineHost _host;
    private readonly ILogger<ProductionViewModel> _logger;
    private readonly DispatcherTimer _timer;
    private ProductionFormat _selectedFormat;
    private TransitionKind _selectedTransition = TransitionKind.Mix;
    private uint _durationFrames = 30;
    private string _status = "Add inputs, then start production.";
    private string? _error;
    private double _transitionProgress;
    private bool _transitionRunning;
    private PipChoice _selectedPip;
    private bool _active;
    private int _statsCountdown;
    private bool _useGpu;
    private CaptureDevice? _selectedCaptureDevice;

    public ProductionViewModel(ProductionService production, EngineHost host, ILogger<ProductionViewModel> logger)
    {
        _production = production;
        _host = host;
        _logger = logger;
        _selectedFormat = production.Format;
        _selectedPip = PipChoice.None;
        PipChoices.Add(PipChoice.None);

        StartStopCommand = new AsyncCommand(StartStopAsync, () => _host.State == EngineHostState.Running);
        CutCommand = new RelayCommand(_ => Guard(() => _production.Cut()), _ => IsRunning);
        AutoCommand = new RelayCommand(_ => Guard(Auto), _ => IsRunning);
        AddTestSignalCommand = new RelayCommand(_ => Guard(() => _production.AddTestSignal()), _ => CanEditInputs);
        AddMediaFileCommand = new RelayCommand(_ => Guard(AddMediaFile), _ => CanEditInputs);
        AddCaptureDeviceCommand = new RelayCommand(_ => Guard(() => _production.AddCaptureDevice(_selectedCaptureDevice!)),
            _ => CanEditInputs && _selectedCaptureDevice is not null);
        RefreshCaptureDevicesCommand = new RelayCommand(_ => Guard(RefreshCaptureDevices), _ => _host.State == EngineHostState.Running);

        _timer = new DispatcherTimer(DispatcherPriority.Render) { Interval = MonitorInterval };
        _timer.Tick += (_, _) => OnTimer();
        _production.Changed += (_, _) => SyncFromService();
        _host.StateChanged += (_, state) => Application.Current?.Dispatcher.InvokeAsync(() =>
        {
            _production.HandleEngineStateChanged(state);
            if (state == EngineHostState.Running) Guard(RefreshCaptureDevices);
            CommandManager.InvalidateRequerySuggested();
            StartStopCommand.RaiseCanExecuteChanged();
        });
        SyncFromService();
    }

    public BusMonitor Preview { get; } = new(VideoBus.Preview);
    public BusMonitor Program { get; } = new(VideoBus.Program);
    public ObservableCollection<InputTileViewModel> Inputs { get; } = [];
    public ObservableCollection<PipChoice> PipChoices { get; } = [];
    public IReadOnlyList<ProductionFormat> Formats => ProductionFormat.Standard;
    public IReadOnlyList<TransitionKind> Transitions { get; } = [TransitionKind.Mix, TransitionKind.Fade, TransitionKind.Wipe];

    public AsyncCommand StartStopCommand { get; }
    public ICommand CutCommand { get; }
    public ICommand AutoCommand { get; }
    public ICommand AddTestSignalCommand { get; }
    public ICommand AddMediaFileCommand { get; }
    public ICommand AddCaptureDeviceCommand { get; }
    public ICommand RefreshCaptureDevicesCommand { get; }

    /// <summary>Webcams / capture cards found by the last refresh (EXPERIMENTAL).</summary>
    public ObservableCollection<CaptureDevice> CaptureDevices { get; } = [];

    public CaptureDevice? SelectedCaptureDevice
    {
        get => _selectedCaptureDevice;
        set => SetProperty(ref _selectedCaptureDevice, value);
    }

    public bool IsRunning => _production.IsRunning;
    public bool CanEditInputs => _production.CanEditInputs && _host.State == EngineHostState.Running;
    public string StartStopText => IsRunning ? "Stop production" : "Start production";

    public ProductionFormat SelectedFormat
    {
        get => _selectedFormat;
        set
        {
            if (value is null || !SetProperty(ref _selectedFormat, value)) return;
            Guard(() => _production.SetFormat(value, Preference));
        }
    }

    /// <summary>EXPERIMENTAL: Direct3D 11 compositor (falls back to CPU if unavailable).</summary>
    public bool UseGpuCompositor
    {
        get => _useGpu;
        set
        {
            if (!SetProperty(ref _useGpu, value)) return;
            Guard(() => _production.SetFormat(_selectedFormat, Preference));
        }
    }

    public TransitionKind SelectedTransition
    {
        get => _selectedTransition;
        set => SetProperty(ref _selectedTransition, value);
    }

    /// <summary>AUTO duration in output frames (1..600).</summary>
    public uint DurationFrames
    {
        get => _durationFrames;
        set => SetProperty(ref _durationFrames, Math.Clamp(value, 1u, 600u));
    }

    public PipChoice SelectedPip
    {
        get => _selectedPip;
        set
        {
            if (value is null || !SetProperty(ref _selectedPip, value)) return;
            if (_production.EngineAvailable) Guard(() => _production.SetPictureInPicture(value.InputId));
        }
    }

    public double TransitionProgress
    {
        get => _transitionProgress;
        private set => SetProperty(ref _transitionProgress, value);
    }

    public bool TransitionRunning
    {
        get => _transitionRunning;
        private set => SetProperty(ref _transitionRunning, value);
    }

    public string Status
    {
        get => _status;
        private set => SetProperty(ref _status, value);
    }

    /// <summary>Last operator-facing error (null = none).</summary>
    public string? Error
    {
        get => _error;
        private set => SetProperty(ref _error, value);
    }

    /// <summary>Monitors refresh only while the section is visible.</summary>
    public bool IsActive
    {
        get => _active;
        set
        {
            if (!SetProperty(ref _active, value)) return;
            UpdateTimer();
        }
    }

    private CompositorPreference Preference => _useGpu ? CompositorPreference.Direct3D11 : CompositorPreference.Auto;

    /// <summary>Same as the Start/Stop button (used by the self-test).</summary>
    internal Task ToggleProductionAsync() => StartStopAsync();

    private async Task StartStopAsync()
    {
        Error = null;
        try
        {
            if (IsRunning)
            {
                await _production.StopAsync();
            }
            else
            {
                if (_production.Inputs.Count == 0) _production.AddTestSignal();  // something to look at
                await _production.StartAsync();
                if (_selectedPip.InputId is uint pip) _production.SetPictureInPicture(pip);
            }
        }
        catch (Exception ex) when (ex is MixItNativeException or InvalidOperationException)
        {
            ReportError(ex);
        }
    }

    private void Auto()
    {
        if (!_production.Auto(_selectedTransition, _durationFrames)) Status = "A transition is already running.";
    }

    private void AddMediaFile()
    {
        var dialog = new OpenFileDialog
        {
            Title = "Add media file",
            Filter = "Media files|*.mp4;*.mov;*.mkv;*.avi;*.mxf;*.ts;*.m4v;*.webm;*.mp3;*.wav;*.m4a|All files|*.*",
        };
        if (dialog.ShowDialog() == true) _production.AddMediaFile(dialog.FileName, loop: true);
    }

    private void RefreshCaptureDevices()
    {
        CaptureDevices.Clear();
        foreach (CaptureDevice d in _production.GetCaptureDevices()) CaptureDevices.Add(d);
        SelectedCaptureDevice = CaptureDevices.FirstOrDefault();
        if (CaptureDevices.Count == 0) Status = "No capture devices found.";
    }

    private void SyncFromService()
    {
        Inputs.Clear();
        var pipKeep = _selectedPip.InputId;
        PipChoices.Clear();
        PipChoices.Add(PipChoice.None);
        int n = 1;
        foreach (ProductionInput input in _production.Inputs)
        {
            uint id = input.Id;
            Inputs.Add(new InputTileViewModel(input)
            {
                Number = (n++).ToString(System.Globalization.CultureInfo.InvariantCulture),
                SelectPreview = new RelayCommand(_ => Guard(() => _production.SetPreview(id)), _ => IsRunning),
                TakeToProgram = new RelayCommand(_ => Guard(() => _production.SetProgram(id)), _ => IsRunning),
                Remove = new RelayCommand(_ => Guard(() => _production.RemoveInput(id)), _ => CanEditInputs),
            });
            PipChoices.Add(new PipChoice(input.Name, input.Id));
        }
        _selectedPip = PipChoices.FirstOrDefault(c => c.InputId == pipKeep) ?? PipChoice.None;
        OnPropertyChanged(nameof(SelectedPip));
        if (!_production.IsRunning)
        {
            Preview.Clear();
            Program.Clear();
            TransitionRunning = false;
            TransitionProgress = 0;
            Status = _production.Inputs.Count == 0 ? "Add inputs, then start production." : $"{_production.Inputs.Count} input(s) ready — stopped.";
        }
        OnPropertyChanged(nameof(IsRunning));
        OnPropertyChanged(nameof(CanEditInputs));
        OnPropertyChanged(nameof(StartStopText));
        CommandManager.InvalidateRequerySuggested();
        UpdateTimer();
    }

    private void UpdateTimer()
    {
        bool run = _active && _production.IsRunning;
        if (run && !_timer.IsEnabled) _timer.Start();
        if (!run && _timer.IsEnabled) _timer.Stop();
    }

    private void OnTimer()
    {
        if (!_production.IsRunning) return;
        try
        {
            ProductionFormat f = _production.Format;
            Preview.Refresh(_production, (int)f.Width, (int)f.Height);
            Program.Refresh(_production, (int)f.Width, (int)f.Height);
            if (--_statsCountdown <= 0)
            {
                _statsCountdown = 5;  // 5 Hz is plenty for tally and counters
                UpdateStatistics();
            }
        }
        catch (MixItNativeException ex)
        {
            ReportError(ex);
        }
    }

    private void UpdateStatistics()
    {
        SessionStatistics? maybe = _production.GetStatistics();
        if (maybe is not { } s) return;
        foreach (InputTileViewModel tile in Inputs)
        {
            tile.OnProgram = tile.Id == s.ProgramInputId;
            tile.OnPreview = tile.Id == s.PreviewInputId;
        }
        Program.Source = NameOf(s.ProgramInputId);
        Preview.Source = NameOf(s.PreviewInputId);
        TransitionRunning = s.ActiveTransition is not null;
        TransitionProgress = s.ActiveTransition is not null ? s.TransitionProgress : 0;
        string compositor = s.Compositor == CompositorKind.Direct3D11 ? "GPU (D3D11)" : "CPU";
        string warning = s.CompositorFallbacks > 0 ? " — GPU failed, running on CPU" : "";
        // "Running", not "on air": nothing is streamed or recorded yet.
        Status = $"RUNNING · {_production.Format.Name} · compositor {compositor}{warning} · " +
                 $"late frames {s.LateTicks} · dropped {s.ProgramFramesDropped} · black {s.ProgramBlackFrames}";
    }

    private string NameOf(uint id) => id == 0 ? "Black" : _production.Inputs.FirstOrDefault(i => i.Id == id)?.Name ?? $"Input {id}";

    private void Guard(Action action)
    {
        Error = null;
        try
        {
            action();
        }
        catch (Exception ex) when (ex is MixItNativeException or InvalidOperationException or ArgumentException)
        {
            ReportError(ex);
        }
    }

    private void ReportError(Exception ex)
    {
        _logger.LogWarning(ex, "Production operation failed");
        Error = ex.Message;
    }
}

/// <summary>Picture-in-picture source choice (null id = off).</summary>
public sealed record PipChoice(string Name, uint? InputId)
{
    public static PipChoice None { get; } = new("Off", null);

    public override string ToString() => Name;
}
