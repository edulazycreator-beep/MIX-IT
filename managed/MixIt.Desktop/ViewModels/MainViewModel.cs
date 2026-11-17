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

    public MainViewModel(EngineHost host, DiagnosticsViewModel diagnostics)
    {
        _host = host;
        Diagnostics = diagnostics;
        Sections =
        [
            new("Production Studio", "Planned — Milestone 3", false),
            new("Input Manager", "Planned — Milestone 3", false),
            new("Audio Mixer", "Planned — Milestone 4", false),
            new("Streaming", "Planned — Milestone 5", false),
            new("Recording", "Planned — Milestone 5", false),
            new("Graphics Studio", "Planned — Milestone 6", false),
            new("Replay", "Planned — Milestone 6", false),
            new("Virtual Studio", "Planned — Milestone 6", false),
            new("Automation", "Planned — Milestone 7", false),
            new("Hardware Diagnostics", "Available", true),
            new("Settings", "Planned — Milestone 8", false),
            new("License", "Planned — Milestone 9", false),
        ];
        _selectedSection = Sections.First(s => s.IsAvailable);
        _host.StateChanged += (_, state) => System.Windows.Application.Current?.Dispatcher.InvokeAsync(() => UpdateStatus(state));
    }

    public IReadOnlyList<SectionItem> Sections { get; }

    public DiagnosticsViewModel Diagnostics { get; }

    public SectionItem SelectedSection
    {
        get => _selectedSection;
        set
        {
            if (SetProperty(ref _selectedSection, value)) OnPropertyChanged(nameof(ShowDiagnostics));
        }
    }

    public bool ShowDiagnostics => SelectedSection.Title == "Hardware Diagnostics";

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
