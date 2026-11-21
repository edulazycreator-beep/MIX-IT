using MixIt.Core.Engine;

namespace MixIt.Desktop.ViewModels;

/// <summary>One entry of the application's section navigation.</summary>
/// <param name="Title">Section name.</param>
/// <param name="Status">Honest implementation status shown to the operator.</param>
/// <param name="IsAvailable">True only if the section has a working backend.</param>
public sealed record SectionItem(string Title, string Status, bool IsAvailable);

/// <summary>Shell view model: navigation and global engine status.</summary>
public sealed class MainViewModel : ObservableObject
{
    private readonly EngineHost _host;
    private SectionItem _selectedSection;
    private string _engineStatus = "Engine stopped";
    private bool _engineHealthy;

    public MainViewModel(EngineHost host, DiagnosticsViewModel diagnostics, ProductionViewModel production,
        AudioMixerViewModel audioMixer, OutputsViewModel outputs, GraphicsViewModel graphics, ReplayViewModel replay, ControlViewModel control,
        SettingsViewModel settings, StatusBarViewModel statusBar)
    {
        _host = host;
        Diagnostics = diagnostics;
        Production = production;
        AudioMixer = audioMixer;
        Outputs = outputs;
        Graphics = graphics;
        Replay = replay;
        Control = control;
        Settings = settings;
        StatusBar = statusBar;
        settings.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(SettingsViewModel.ShowName)) OnPropertyChanged(nameof(WindowTitle));
        };
        Sections =
        [
            new("Production Studio", "Available (M3)", true),
            new("Input Manager", "Planned — inputs are added in Production Studio", false),
            new("Audio Mixer", "Available — mixer, EQ, dynamics, meters (M4)", true),
            new("Streaming & Recording", "Available — record, ISO, RTMP/RTMPS/SRT/HLS (M5)", true),
            new("Graphics Studio", "Available — titles, lower thirds, clock, scoreboard, overlay channels, keys (M6)", true),
            new("Replay & Multiview", "Available — multiview, instant replay with slow motion (M6)", true),
            new("Virtual Studio", "Basic — in Graphics Studio: a keyed camera (green/blue screen) placed on an overlay channel over a background input. No 3D sets or camera tracking", false),
            new("Automation & Control", "Available — macros, automation, remote control API, tally, PTZ, extensions (M7)", true),
            new("Hardware Diagnostics", "Available", true),
            new("Settings & Workspace", "Available — show files, shortcuts, full-screen outputs (M8)", true),
            new("License", "Planned — Milestone 9", false),
        ];
        _selectedSection = Sections.FirstOrDefault(s => s.IsAvailable && s.Title == settings.Workspace.LastSection) ?? Sections[0];
        Production.IsActive = ShowProduction;
        _host.StateChanged += (_, state) => System.Windows.Application.Current?.Dispatcher.InvokeAsync(() => UpdateStatus(state));
    }

    public IReadOnlyList<SectionItem> Sections { get; }

    public DiagnosticsViewModel Diagnostics { get; }

    public ProductionViewModel Production { get; }

    public AudioMixerViewModel AudioMixer { get; }

    public OutputsViewModel Outputs { get; }

    public GraphicsViewModel Graphics { get; }

    public ReplayViewModel Replay { get; }

    public ControlViewModel Control { get; }

    public SettingsViewModel Settings { get; }

    public StatusBarViewModel StatusBar { get; }

    public string WindowTitle => $"MIX IT — {Settings.ShowName}";

    public SectionItem SelectedSection
    {
        get => _selectedSection;
        set
        {
            if (!SetProperty(ref _selectedSection, value)) return;
            OnPropertyChanged(nameof(ShowDiagnostics));
            OnPropertyChanged(nameof(ShowProduction));
            OnPropertyChanged(nameof(ShowAudioMixer));
            OnPropertyChanged(nameof(ShowOutputs));
            OnPropertyChanged(nameof(ShowGraphics));
            OnPropertyChanged(nameof(ShowReplay));
            OnPropertyChanged(nameof(ShowControl));
            OnPropertyChanged(nameof(ShowSettings));
            OnPropertyChanged(nameof(ShowPlaceholder));
            Production.IsActive = ShowProduction;  // monitors and meters poll only while visible
            AudioMixer.IsActive = ShowAudioMixer;
            Outputs.IsActive = ShowOutputs;
            Graphics.IsActive = ShowGraphics;
            Replay.IsActive = ShowReplay;
            Control.IsActive = ShowControl;
            Settings.SaveWorkspace(w => w with { LastSection = value.Title });
        }
    }

    public bool ShowDiagnostics => SelectedSection.Title == "Hardware Diagnostics";

    public bool ShowProduction => SelectedSection.Title == "Production Studio";

    public bool ShowAudioMixer => SelectedSection.Title == "Audio Mixer";

    public bool ShowOutputs => SelectedSection.Title == "Streaming & Recording";

    public bool ShowGraphics => SelectedSection.Title == "Graphics Studio";

    public bool ShowReplay => SelectedSection.Title == "Replay & Multiview";

    public bool ShowControl => SelectedSection.Title == "Automation & Control";

    public bool ShowSettings => SelectedSection.Title == "Settings & Workspace";

    public bool ShowPlaceholder => !ShowDiagnostics && !ShowProduction && !ShowAudioMixer && !ShowOutputs && !ShowGraphics && !ShowReplay && !ShowControl && !ShowSettings;

    public string EngineStatus
    {
        get => _engineStatus;
        private set => SetProperty(ref _engineStatus, value);
    }

    public bool EngineHealthy
    {
        get => _engineHealthy;
        private set => SetProperty(ref _engineHealthy, value);
    }

    /// <summary>Starts the engine off the UI thread, then loads diagnostics.</summary>
    public Task InitializeAsync() => Diagnostics.StartEngineAsync();

    private void UpdateStatus(EngineHostState state)
    {
        EngineHealthy = state == EngineHostState.Running;
        EngineStatus = state switch
        {
            EngineHostState.Running => "Engine running",
            EngineHostState.Faulted => "Engine fault — see Hardware Diagnostics",
            _ => "Engine stopped",
        };
    }
}
