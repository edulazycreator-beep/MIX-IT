using System.Collections.ObjectModel;
using System.Globalization;
using System.Windows.Input;
using System.Windows.Threading;
using Microsoft.Extensions.Logging;
using MixIt.Core.Graphics;
using MixIt.Core.Production;
using MixIt.Interop;

namespace MixIt.Desktop.ViewModels;

public sealed record KeyChoice(string Title, KeySettings Key);

public sealed record InputChoice(uint? InputId, string Title);

/// <summary>
/// Graphics Studio (M6): titles from templates with live fields and timers,
/// and the four overlay channels (downstream keys) that put graphics, logos
/// or keyed cameras on top of the program with a fade.
/// </summary>
public sealed class GraphicsViewModel : ObservableObject
{
    private readonly ProductionService _production;
    private readonly ILogger<GraphicsViewModel> _logger;
    private readonly DispatcherTimer _timer;
    private GraphicsTemplate _template = GraphicsTemplates.All[0];
    private string _logoPath = "";
    private string? _error;
    private bool _active;

    public static IReadOnlyList<KeyChoice> Keys { get; } =
    [
        new("No key", KeySettings.None),
        new("Green screen", KeySettings.GreenScreen),
        new("Blue screen", KeySettings.BlueScreen),
        new("Luma (black out)", new KeySettings { Type = KeyType.Luma, Threshold = 0.08f, Softness = 0.06f }),
    ];

    public GraphicsViewModel(ProductionService production, ILogger<GraphicsViewModel> logger)
    {
        _production = production;
        _logger = logger;
        AddTemplateCommand = new RelayCommand(_ => Guard(() =>
        {
            ProductionInput input = _production.AddGraphics(_template.Name, _template.SceneJson, _template.Fields);
            _logger.LogInformation("Graphics {Name} added as input {Id}", _template.Name, input.Id);
        }), _ => _production.EngineAvailable && _production.CanEditInputs);
        AddLogoCommand = new RelayCommand(_ => Guard(() =>
        {
            GraphicsTemplate logo = GraphicsTemplates.Logo(_logoPath.Trim());
            _production.AddGraphics("Logo", logo.SceneJson);
        }), _ => _production.EngineAvailable && _production.CanEditInputs && _logoPath.Trim().Length > 0);
        for (int c = 0; c < NativeSession.OverlayChannels; c++) Channels.Add(new OverlayChannelViewModel(c, this));
        _timer = new DispatcherTimer(DispatcherPriority.Background) { Interval = TimeSpan.FromMilliseconds(250) };
        _timer.Tick += (_, _) => Poll();
        _production.Changed += (_, _) => Sync();
        Sync();
    }

    public IReadOnlyList<GraphicsTemplate> Templates => GraphicsTemplates.All;
    public ObservableCollection<GraphicsItemViewModel> Items { get; } = [];
    public ObservableCollection<OverlayChannelViewModel> Channels { get; } = [];
    public ObservableCollection<InputChoice> InputChoices { get; } = [];
    public ICommand AddTemplateCommand { get; }
    public ICommand AddLogoCommand { get; }

    public GraphicsTemplate Template
    {
        get => _template;
        set
        {
            if (value is not null) SetProperty(ref _template, value);
        }
    }

    public string LogoPath
    {
        get => _logoPath;
        set => SetProperty(ref _logoPath, value ?? "");
    }

    public string? Error
    {
        get => _error;
        private set => SetProperty(ref _error, value);
    }

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
            _logger.LogWarning("Graphics operation failed: {Message}", ex.Message);
            Error = ex.Message;
        }
    }

    private void Sync()
    {
        Items.Clear();
        InputChoices.Clear();
        InputChoices.Add(new InputChoice(null, "(empty)"));
        if (!_production.EngineAvailable) return;
        foreach (ProductionInput input in _production.Inputs)
        {
            InputChoices.Add(new InputChoice(input.Id, input.Name));
            if (input.Kind == ProductionInputKind.Graphics) Items.Add(new GraphicsItemViewModel(input, this));
        }
        foreach (OverlayChannelViewModel c in Channels) c.Refresh();
    }

    private void Poll()
    {
        foreach (GraphicsItemViewModel i in Items) i.RefreshInfo();
        foreach (OverlayChannelViewModel c in Channels) c.RefreshState();
    }
}

/// <summary>One graphics input: its fields (edited live) and timer.</summary>
public sealed class GraphicsItemViewModel : ObservableObject
{
    private readonly ProductionInput _input;
    private readonly GraphicsViewModel _owner;
    private string _timerText = "";
    private string _warnings = "";
    private int _timerSeconds = 300;
    private GraphicsTimerMode _timerMode = GraphicsTimerMode.Countdown;

    internal GraphicsItemViewModel(ProductionInput input, GraphicsViewModel owner)
    {
        _input = input;
        _owner = owner;
        try
        {
            foreach ((string name, string value) in owner.Production.GetGraphicsFields(input.Id)) Fields.Add(new GraphicsFieldViewModel(this, name, value));
        }
        catch (ArgumentException)
        {
            // Not a graphics input any more (list being rebuilt).
        }
        StartTimerCommand = new RelayCommand(_ => _owner.Guard(() => _owner.Production.SetGraphicsTimer(Id, _timerMode, null, true)));
        PauseTimerCommand = new RelayCommand(_ => _owner.Guard(() => _owner.Production.SetGraphicsTimer(Id, _timerMode, null, false)));
        ResetTimerCommand = new RelayCommand(_ => _owner.Guard(() =>
            _owner.Production.SetGraphicsTimer(Id, _timerMode,
                _timerMode == GraphicsTimerMode.Countdown ? TimeSpan.FromSeconds(_timerSeconds) : TimeSpan.Zero, false)));
    }

    public uint Id => _input.Id;
    public string Name => _input.Name;
    public ObservableCollection<GraphicsFieldViewModel> Fields { get; } = [];
    public ICommand StartTimerCommand { get; }
    public ICommand PauseTimerCommand { get; }
    public ICommand ResetTimerCommand { get; }
    public IReadOnlyList<GraphicsTimerMode> TimerModes { get; } = [GraphicsTimerMode.Countdown, GraphicsTimerMode.Stopwatch];

    public GraphicsTimerMode TimerMode
    {
        get => _timerMode;
        set => SetProperty(ref _timerMode, value);
    }

    public int TimerSeconds
    {
        get => _timerSeconds;
        set => SetProperty(ref _timerSeconds, Math.Clamp(value, 0, 360_000));
    }

    public string TimerText
    {
        get => _timerText;
        private set => SetProperty(ref _timerText, value);
    }

    public string Warnings
    {
        get => _warnings;
        private set => SetProperty(ref _warnings, value);
    }

    internal void SetField(string name, string value) => _owner.Guard(() => _owner.Production.SetGraphicsField(Id, name, value));

    internal void RefreshInfo()
    {
        try
        {
            GraphicsInfo info = _owner.Production.GetGraphicsInfo(Id);
            TimerText = $"{info.Timer.ToString(info.Timer.TotalHours >= 1 ? @"h\:mm\:ss" : @"m\:ss", CultureInfo.InvariantCulture)}{(info.TimerRunning ? " ▶" : " ⏸")}";
            Warnings = info.Warnings;
        }
        catch (Exception ex) when (ex is MixItNativeException or InvalidOperationException or ArgumentException)
        {
            TimerText = "";
        }
    }
}

/// <summary>A text field of a graphics input; edits go on air immediately.</summary>
public sealed class GraphicsFieldViewModel(GraphicsItemViewModel item, string name, string value) : ObservableObject
{
    private string _value = value;

    public string Name { get; } = name;

    public string Value
    {
        get => _value;
        set
        {
            if (!SetProperty(ref _value, value ?? "")) return;
            item.SetField(Name, _value);
        }
    }
}

/// <summary>Overlay channel: input, key, fade, IN/OUT and on-air state.</summary>
public sealed class OverlayChannelViewModel : ObservableObject
{
    private readonly GraphicsViewModel _owner;
    private InputChoice? _input;
    private KeyChoice _key = GraphicsViewModel.Keys[0];
    private int _fadeFrames = 15;
    private string _state = "";
    private bool _onAir;
    private bool _syncing;

    internal OverlayChannelViewModel(int channel, GraphicsViewModel owner)
    {
        Channel = channel;
        _owner = owner;
        InCommand = new RelayCommand(_ => _owner.Guard(() => _owner.Production.ShowOverlay(Channel, true, _fadeFrames)), _ => _input?.InputId is not null);
        OutCommand = new RelayCommand(_ => _owner.Guard(() => _owner.Production.ShowOverlay(Channel, false, _fadeFrames)), _ => _input?.InputId is not null);
        CutCommand = new RelayCommand(_ => _owner.Guard(() => _owner.Production.ShowOverlay(Channel, !_onAir, 0)), _ => _input?.InputId is not null);
    }

    public int Channel { get; }
    public string Title => $"Overlay {Channel + 1}";
    public ObservableCollection<InputChoice> Inputs => _owner.InputChoices;
    public IReadOnlyList<KeyChoice> Keys => GraphicsViewModel.Keys;
    public ICommand InCommand { get; }
    public ICommand OutCommand { get; }
    public ICommand CutCommand { get; }

    public InputChoice? Input
    {
        get => _input;
        set
        {
            if (!SetProperty(ref _input, value) || _syncing) return;
            Apply();
        }
    }

    public KeyChoice Key
    {
        get => _key;
        set
        {
            if (value is null || !SetProperty(ref _key, value) || _syncing) return;
            Apply();
        }
    }

    public int FadeFrames
    {
        get => _fadeFrames;
        set => SetProperty(ref _fadeFrames, Math.Clamp(value, 0, 600));
    }

    public string State
    {
        get => _state;
        private set => SetProperty(ref _state, value);
    }

    public bool OnAir
    {
        get => _onAir;
        private set => SetProperty(ref _onAir, value);
    }

    private void Apply() => _owner.Guard(() => _owner.Production.SetOverlay(Channel, _input?.InputId, _key.Key));

    internal void Refresh()
    {
        _syncing = true;
        try
        {
            (uint? id, KeySettings key, _) = _owner.Production.GetOverlay(Channel);
            Input = Inputs.FirstOrDefault(i => i.InputId == id) ?? Inputs.FirstOrDefault();
            Key = Keys.FirstOrDefault(k => k.Key == key) ?? Keys[0];
        }
        catch (Exception ex) when (ex is MixItNativeException or InvalidOperationException or ArgumentException)
        {
            // Engine not available: leave the controls as they are.
        }
        finally
        {
            _syncing = false;
        }
        RefreshState();
    }

    internal void RefreshState()
    {
        try
        {
            OverlayState s = _owner.Production.GetOverlay(Channel).State;
            OnAir = s.Level > 0;
            State = s.InputId == 0 ? "empty" : s.Level >= 1 ? "ON AIR" : s.Level > 0 ? $"fading {s.Level:P0}" : "off";
        }
        catch (Exception ex) when (ex is MixItNativeException or InvalidOperationException or ArgumentException)
        {
            State = "";
        }
    }
}
