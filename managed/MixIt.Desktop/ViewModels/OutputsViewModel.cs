using System.Collections.ObjectModel;
using System.Globalization;
using System.IO;
using System.Windows.Input;
using System.Windows.Threading;
using Microsoft.Extensions.Logging;
using MixIt.Core.Production;
using MixIt.Interop;

namespace MixIt.Desktop.ViewModels;

/// <summary>Kinds of destination the operator can add.</summary>
public enum OutputKind
{
    RecordFile,
    StreamRtmp,
    StreamSrt,
    HlsFolder,
}

public sealed record OutputKindChoice(OutputKind Kind, string Title, string UrlHint);

/// <summary>Program, the multiview, or an input for ISO recording.</summary>
public sealed record OutputSourceChoice(uint? InputId, string Title)
{
    public bool IsMultiview => InputId == OutputSettings.MultiviewSource;
}

/// <summary>
/// Streaming &amp; Recording (M5): destinations for the program (or one input,
/// ISO), each started and stopped independently. State, bitrate, buffering,
/// drops and errors are polled twice a second while visible. Stream keys are
/// entered separately and never shown again (only a redacted address).
/// </summary>
public sealed class OutputsViewModel : ObservableObject
{
    private readonly ProductionService _production;
    private readonly ILogger<OutputsViewModel> _logger;
    private readonly DispatcherTimer _timer;
    private bool _active;
    private string? _error;
    private OutputKindChoice _kind;
    private string _name = "Program recording";
    private string _address = "";
    private string _streamKey = "";
    private OutputSourceChoice _source = new(null, "Program");
    private int _videoKbps = 6000;
    private int _segmentMinutes;
    private string _encoder = AutoEncoder;
    private string _encoderStatus = "Not checked yet";

    public const string AutoEncoder = "Automatic (hardware first)";

    public OutputsViewModel(ProductionService production, ILogger<OutputsViewModel> logger)
    {
        _production = production;
        _logger = logger;
        Kinds =
        [
            new(OutputKind.RecordFile, "Record to file", "File path (.mp4, .mov, .mkv, .ts)"),
            new(OutputKind.StreamRtmp, "Stream (RTMP / RTMPS)", "Server URL, e.g. rtmps://live.example.com/app"),
            new(OutputKind.StreamSrt, "Stream (SRT)", "srt://host:port?mode=caller (passphrase in the URL)"),
            new(OutputKind.HlsFolder, "HLS to a folder", "Playlist path ending in .m3u8"),
        ];
        _kind = Kinds[0];
        _address = DefaultRecordingPath("Program.mp4");
        Encoders.Add(AutoEncoder);
        _timer = new DispatcherTimer(DispatcherPriority.Background) { Interval = TimeSpan.FromMilliseconds(500) };
        _timer.Tick += (_, _) => Poll();
        _production.Changed += (_, _) => Sync();
        AddCommand = new RelayCommand(_ => Guard(Add), _ => _production.EngineAvailable && _address.Trim().Length > 0 && _name.Trim().Length > 0);
        CheckEncodersCommand = new AsyncCommand(CheckEncodersAsync, () => _production.EngineAvailable);
        Sync();
    }

    public IReadOnlyList<OutputKindChoice> Kinds { get; }
    public ObservableCollection<OutputSourceChoice> Sources { get; } = [];
    public ObservableCollection<string> Encoders { get; } = [];
    public ObservableCollection<OutputRowViewModel> Outputs { get; } = [];
    public ICommand AddCommand { get; }
    public AsyncCommand CheckEncodersCommand { get; }

    public OutputKindChoice Kind
    {
        get => _kind;
        set
        {
            if (value is null || !SetProperty(ref _kind, value)) return;
            Address = value.Kind switch
            {
                OutputKind.RecordFile => DefaultRecordingPath("Program.mp4"),
                OutputKind.HlsFolder => DefaultRecordingPath(Path.Combine("HLS", "live.m3u8")),
                _ => "",
            };
            Name = value.Kind switch
            {
                OutputKind.RecordFile => "Program recording",
                OutputKind.HlsFolder => "HLS",
                _ => "Stream",
            };
            OnPropertyChanged(nameof(IsRtmp));
            OnPropertyChanged(nameof(IsFile));
        }
    }

    public bool IsRtmp => _kind.Kind == OutputKind.StreamRtmp;
    public bool IsFile => _kind.Kind == OutputKind.RecordFile;

    public string Name
    {
        get => _name;
        set => SetProperty(ref _name, value ?? "");
    }

    public string Address
    {
        get => _address;
        set => SetProperty(ref _address, value ?? "");
    }

    /// <summary>Set from the password box; never read back into the UI.</summary>
    public string StreamKey
    {
        set => _streamKey = value ?? "";
    }

    public OutputSourceChoice Source
    {
        get => _source;
        set
        {
            if (value is not null) SetProperty(ref _source, value);
        }
    }

    public int VideoKbps
    {
        get => _videoKbps;
        set => SetProperty(ref _videoKbps, Math.Clamp(value, 500, 50_000));
    }

    public int SegmentMinutes
    {
        get => _segmentMinutes;
        set => SetProperty(ref _segmentMinutes, Math.Clamp(value, 0, 600));
    }

    public string Encoder
    {
        get => _encoder;
        set => SetProperty(ref _encoder, value ?? AutoEncoder);
    }

    public string EncoderStatus
    {
        get => _encoderStatus;
        private set => SetProperty(ref _encoderStatus, value);
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
            _logger.LogWarning("Output operation failed: {Message}", ex.Message);
            Error = ex.Message;
        }
    }

    private static string DefaultRecordingPath(string file) =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyVideos), "MIX IT", file);

    private void Add()
    {
        string address = _address.Trim();
        string url = _kind.Kind == OutputKind.StreamRtmp && _streamKey.Length > 0
            ? address.TrimEnd('/') + "/" + _streamKey.Trim()
            : address;
        var settings = new OutputSettings
        {
            Url = url,
            VideoKbps = _videoKbps,
            VideoEncoder = _encoder == AutoEncoder ? null : _encoder,
            SegmentSeconds = _kind.Kind == OutputKind.RecordFile ? _segmentMinutes * 60 : 0,
            SourceInputId = _source.IsMultiview ? OutputSettings.MultiviewSource : 0,
        };
        _production.AddOutput(_name.Trim(), settings, _source.IsMultiview ? null : _source.InputId);
        _streamKey = "";
    }

    private async Task CheckEncodersAsync()
    {
        EncoderStatus = "Testing encoders…";
        IReadOnlyList<VideoEncoderInfo> list = await _production.QueryEncodersAsync();
        Encoders.Clear();
        Encoders.Add(AutoEncoder);
        foreach (VideoEncoderInfo e in list.Where(e => e.Usable)) Encoders.Add(e.Name);
        Encoder = AutoEncoder;
        VideoEncoderInfo? best = list.FirstOrDefault(e => e.Usable);
        EncoderStatus = best is null
            ? "No usable H.264 encoder found"
            : $"Automatic uses {best.Name} ({(best.Hardware ? "hardware" : "software")}); {list.Count(e => e.Usable)} usable";
    }

    private void Sync()
    {
        uint? keep = _source.InputId;
        Sources.Clear();
        Sources.Add(new OutputSourceChoice(null, "Program"));
        Sources.Add(new OutputSourceChoice(OutputSettings.MultiviewSource, "Multiview (with program audio)"));
        foreach (ProductionInput input in _production.Inputs) Sources.Add(new OutputSourceChoice(input.Id, $"ISO: {input.Name}"));
        Source = Sources.FirstOrDefault(s => s.InputId == keep) ?? Sources[0];
        Outputs.Clear();
        foreach (ProductionOutput o in _production.Outputs) Outputs.Add(new OutputRowViewModel(o, this));
        Poll();
    }

    private void Poll()
    {
        foreach (OutputRowViewModel row in Outputs) row.Refresh();
    }
}

/// <summary>One destination: state, live figures and its commands.</summary>
public sealed class OutputRowViewModel : ObservableObject
{
    private readonly ProductionOutput _output;
    private readonly OutputsViewModel _owner;
    private OutputStatistics? _stats;

    internal OutputRowViewModel(ProductionOutput output, OutputsViewModel owner)
    {
        _output = output;
        _owner = owner;
        StartCommand = new RelayCommand(_ => _owner.Guard(() => _owner.Production.StartOutput(_output.Id)),
            _ => _owner.Production.IsRunning && !IsBusy);
        StopCommand = new RelayCommand(_ => _owner.Guard(() => _owner.Production.StopOutput(_output.Id)), _ => IsBusy);
        RemoveCommand = new RelayCommand(_ => _owner.Guard(() => _owner.Production.RemoveOutput(_output.Id)), _ => !IsBusy);
    }

    public string Name => _output.Name;
    public string Kind => _output.SourceInputId is not null ? "ISO"
        : _output.IsMultiview ? "Multiview"
        : _output.Settings.IsNetwork ? "Stream" : "Recording";
    public string Target => _stats is { CurrentFile.Length: > 0 } s ? s.CurrentFile : UrlRedaction.Redact(_output.Settings.Url);
    public ICommand StartCommand { get; }
    public ICommand StopCommand { get; }
    public ICommand RemoveCommand { get; }

    /// <summary>Started and not yet finished (running, connecting, retrying or finalizing).</summary>
    public bool IsBusy => _stats?.State is OutputState.Starting or OutputState.Running or OutputState.Reconnecting or OutputState.Stopping;

    public string State => _stats?.State switch
    {
        null => "Not in session",
        OutputState.Idle => "Ready",
        OutputState.Starting => "Connecting…",
        OutputState.Running => _output.Settings.IsNetwork ? "LIVE" : "REC",
        OutputState.Reconnecting => $"Reconnecting (attempt {_stats.Reconnects})",
        OutputState.Stopping => "Finishing…",
        OutputState.Failed => "Failed",
        _ => "?",
    };

    public bool IsLive => _stats?.State == OutputState.Running;
    public bool IsProblem => _stats is { } s && (s.State is OutputState.Failed or OutputState.Reconnecting || s.DataLost);

    public string Details
    {
        get
        {
            if (_stats is not { } s) return "";
            var parts = new List<string>();
            if (s.Encoder.Length > 0) parts.Add($"{s.Encoder}{(s.HardwareEncoder ? " (hw)" : "")}");
            if (s.State is OutputState.Running or OutputState.Stopping || s.Duration > TimeSpan.Zero)
            {
                parts.Add(s.Duration.ToString(@"hh\:mm\:ss", CultureInfo.InvariantCulture));
                parts.Add($"{s.BitrateKbps} kb/s");
                parts.Add($"{s.BytesWritten / 1_048_576.0:0.0} MB");
            }
            if (s.QueueMilliseconds > 500) parts.Add($"buffer {s.QueueMilliseconds} ms");
            if (s.DroppedPackets > 0) parts.Add($"{s.DroppedPackets} packets dropped");
            if (s.EncoderDroppedFrames > 0) parts.Add($"{s.EncoderDroppedFrames} frames not encoded");
            if (s.Segments > 1) parts.Add($"file {s.Segments}");
            if (s.FreeBytes is { } free) parts.Add($"{free / 1_073_741_824.0:0.0} GB free");
            if (s.DataLost) parts.Add("DATA LOST (drive too slow or full)");
            if (s.LastError.Length > 0 && s.State is OutputState.Failed or OutputState.Reconnecting) parts.Add(s.LastError);
            return string.Join("  ·  ", parts);
        }
    }

    internal void Refresh()
    {
        try
        {
            _stats = _owner.Production.GetOutputStatistics(_output.Id);
        }
        catch (Exception ex) when (ex is MixItNativeException or InvalidOperationException or ArgumentException)
        {
            _stats = null;
        }
        OnPropertyChanged(nameof(State));
        OnPropertyChanged(nameof(Details));
        OnPropertyChanged(nameof(Target));
        OnPropertyChanged(nameof(IsLive));
        OnPropertyChanged(nameof(IsProblem));
        OnPropertyChanged(nameof(IsBusy));
        CommandManager.InvalidateRequerySuggested();
    }
}
