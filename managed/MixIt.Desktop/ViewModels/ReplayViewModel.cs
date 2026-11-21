using System.Collections.ObjectModel;
using System.Globalization;
using System.Windows.Input;
using System.Windows.Threading;
using Microsoft.Extensions.Logging;
using MixIt.Core.Production;
using MixIt.Interop;

namespace MixIt.Desktop.ViewModels;

public sealed record SpeedChoice(string Title, float Speed);

/// <summary>
/// Replay &amp; Multiview (M6): the operator multiview monitor, and instant
/// replay inputs (record an input's last N seconds, mark an in-point, play it
/// back in slow motion, take it to air). Engine work goes through
/// <see cref="ProductionService"/>; this class only binds and polls.
/// </summary>
public sealed class ReplayViewModel : ObservableObject
{
    private static readonly TimeSpan MonitorInterval = TimeSpan.FromMilliseconds(40);

    private readonly ProductionService _production;
    private readonly ILogger<ReplayViewModel> _logger;
    private readonly DispatcherTimer _timer;
    private InputChoice? _source;
    private int _seconds = 60;
    private string? _error;
    private bool _active;
    private int _pollDivider;

    internal static readonly IReadOnlyList<SpeedChoice> SpeedChoices =
    [
        new("25 %", 0.25f), new("50 %", 0.5f), new("75 %", 0.75f), new("100 %", 1.0f),
    ];

    private static readonly IReadOnlyList<int> LengthChoices = [15, 30, 60, 120, 300];

    // Instance properties: WPF bindings resolve instance members.
    public IReadOnlyList<SpeedChoice> Speeds => SpeedChoices;

    public IReadOnlyList<int> Lengths => LengthChoices;

    public ReplayViewModel(ProductionService production, ILogger<ReplayViewModel> logger)
    {
        _production = production;
        _logger = logger;
        AddReplayCommand = new RelayCommand(_ => Guard(() =>
        {
            if (_source?.InputId is not uint id) throw new InvalidOperationException("Choose the input to record.");
            ProductionInput input = _production.AddReplay(id, _seconds);
            _logger.LogInformation("Replay input {Id} added for input {Source} ({Seconds} s)", input.Id, id, _seconds);
        }), _ => _production.EngineAvailable && _production.CanEditInputs && _source?.InputId is not null);
        _timer = new DispatcherTimer(DispatcherPriority.Render) { Interval = MonitorInterval };
        _timer.Tick += (_, _) => Poll();
        _production.Changed += (_, _) => Sync();
        Sync();
    }

    public BusMonitor Multiview { get; } = new(VideoBus.Multiview);

    public ObservableCollection<ReplayItemViewModel> Items { get; } = [];

    public ObservableCollection<InputChoice> Sources { get; } = [];

    public ICommand AddReplayCommand { get; }

    public InputChoice? Source
    {
        get => _source;
        set
        {
            if (value is not null) SetProperty(ref _source, value);  // null: ComboBox while the list is rebuilt
        }
    }

    public int Seconds
    {
        get => _seconds;
        set => SetProperty(ref _seconds, value);
    }

    public string? Error
    {
        get => _error;
        private set => SetProperty(ref _error, value);
    }

    /// <summary>The multiview is rendered by the engine only while someone reads it: poll only while visible.</summary>
    public bool IsActive
    {
        get => _active;
        set
        {
            if (!SetProperty(ref _active, value)) return;
            if (value) _timer.Start();
            else _timer.Stop();
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
            _logger.LogWarning("Replay operation failed: {Message}", ex.Message);
            Error = ex.Message;
        }
    }

    private void Sync()
    {
        uint? selected = _source?.InputId;
        Items.Clear();
        Sources.Clear();
        foreach (ProductionInput input in _production.Inputs)
        {
            if (input.Kind == ProductionInputKind.Replay) Items.Add(new ReplayItemViewModel(input, this));
            else if (input.Kind != ProductionInputKind.AudioCapture) Sources.Add(new InputChoice(input.Id, input.Name));
        }
        Source = Sources.FirstOrDefault(s => s.InputId == selected) ?? Sources.FirstOrDefault();
        if (!_production.IsRunning) Multiview.Clear();
    }

    private void Poll()
    {
        if (!_production.IsRunning) return;
        Multiview.Refresh(_production, (int)_production.Format.Width, (int)_production.Format.Height);
        if (++_pollDivider % 5 != 0) return;  // replay status at 5 Hz
        foreach (ReplayItemViewModel item in Items) item.Poll();
    }
}

/// <summary>One replay input with its transport controls.</summary>
public sealed class ReplayItemViewModel : ObservableObject
{
    private readonly ReplayViewModel _owner;
    private SpeedChoice _speed = ReplayViewModel.SpeedChoices[1];
    private string _status = "Start production to record.";
    private string _mark = "no mark";
    private bool _paused;

    public ReplayItemViewModel(ProductionInput input, ReplayViewModel owner)
    {
        _owner = owner;
        Id = input.Id;
        Name = input.Name;
        Detail = input.Detail;
        MarkCommand = new RelayCommand(_ => _owner.Guard(() =>
        {
            _owner.Production.MarkReplayIn(Id);
            Poll();
        }), _ => _owner.Production.IsRunning);
        PlayMarkCommand = new RelayCommand(_ => _owner.Guard(() => _owner.Production.PlayReplay(Id, _speed.Speed)),
            _ => _owner.Production.IsRunning);
        PlayLastCommand = new RelayCommand(p => _owner.Guard(() =>
            _owner.Production.PlayReplay(Id, _speed.Speed, double.Parse((string)p!, CultureInfo.InvariantCulture), fromMark: false)),
            _ => _owner.Production.IsRunning);
        PauseCommand = new RelayCommand(_ => _owner.Guard(() => _owner.Production.ControlReplay(Id, null, !_paused)),
            _ => _owner.Production.IsRunning);
        PreviewCommand = new RelayCommand(_ => _owner.Guard(() => _owner.Production.SetPreview(Id)), _ => _owner.Production.IsRunning);
        RemoveCommand = new RelayCommand(_ => _owner.Guard(() => _owner.Production.RemoveInput(Id)), _ => _owner.Production.CanEditInputs);
    }

    public uint Id { get; }
    public string Name { get; }
    public string Detail { get; }
    public ICommand MarkCommand { get; }
    public ICommand PlayMarkCommand { get; }
    public ICommand PlayLastCommand { get; }
    public ICommand PauseCommand { get; }
    public ICommand PreviewCommand { get; }
    public ICommand RemoveCommand { get; }

    public SpeedChoice Speed
    {
        get => _speed;
        set
        {
            if (value is null || !SetProperty(ref _speed, value)) return;
            if (_owner.Production.IsRunning) _owner.Guard(() => _owner.Production.ControlReplay(Id, value.Speed, null));
        }
    }

    public string Status
    {
        get => _status;
        private set => SetProperty(ref _status, value);
    }

    public string Mark
    {
        get => _mark;
        private set => SetProperty(ref _mark, value);
    }

    public string PauseLabel => _paused ? "RESUME" : "PAUSE";

    internal void Poll()
    {
        ReplayStatus s;
        try
        {
            s = _owner.Production.GetReplayStatus(Id);
        }
        catch (Exception ex) when (ex is MixItNativeException or InvalidOperationException or ArgumentException)
        {
            Status = ex.Message;
            return;
        }
        double recorded = (s.Buffer.NewestNs - s.Buffer.OldestNs) / 1e9;
        string buffer = s.Buffer.Frames == 0
            ? "buffer empty"
            : string.Create(CultureInfo.InvariantCulture,
                $"buffer {recorded:0.0} s, {s.Buffer.Bytes / 1_048_576.0:0} MB{(s.Buffer.Dropped > 0 ? $", {s.Buffer.Dropped} frames not recorded" : "")}");
        string playback = !s.State.Active
            ? "idle"
            : string.Create(CultureInfo.InvariantCulture,
                $"{(s.State.Paused ? "paused" : "playing")} at {s.State.Speed * 100:0} %, {(s.Buffer.NewestNs - s.State.PositionNs) / 1e9:0.0} s behind live");
        Status = $"{buffer} · {playback}";
        Mark = s.MarkNs is { } m
            ? string.Create(CultureInfo.InvariantCulture, $"mark {(s.Buffer.NewestNs - m) / 1e9:0.0} s ago{(m < s.Buffer.OldestNs ? " (expired)" : "")}")
            : "no mark";
        if (_paused != (s.State.Active && s.State.Paused))
        {
            _paused = s.State.Active && s.State.Paused;
            OnPropertyChanged(nameof(PauseLabel));
        }
    }
}
