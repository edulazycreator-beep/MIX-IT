using System.Text.Json;
using System.Text.Json.Serialization;
using MixIt.Interop;

namespace MixIt.Core.Production;

/// <summary>
/// A saved production ("show file", M8): format, inputs with their names,
/// audio strips, graphics scenes and fields, replay buffers, overlay channels,
/// picture-in-picture, outputs and audio buses. JSON, versioned.
/// Not saved: plug-in inserts (re-add them; their state belongs to the plug-in),
/// replay marks, what is on air.
/// </summary>
public sealed record ShowDocument
{
    public const int CurrentVersion = 1;

    public int Version { get; init; } = CurrentVersion;
    public string Product { get; init; } = "MIX IT";
    public required ShowFormat Format { get; init; }
    public CompositorPreference Compositor { get; init; }
    public List<ShowInput> Inputs { get; init; } = [];
    public List<ShowOverlay?> Overlays { get; init; } = [];
    public int? PictureInPicture { get; init; }
    public List<ShowOutput> Outputs { get; init; } = [];
    public Dictionary<AudioBus, AudioBusSettings> Buses { get; init; } = [];
    public AudioBus MonitorSource { get; init; } = AudioBus.Master;
}

public sealed record ShowFormat(string Name, uint Width, uint Height, uint RateNumerator, uint RateDenominator);

/// <param name="Path">File path or device id.</param>
/// <param name="ReplayOf">Replay inputs: index of the input they replay.</param>
public sealed record ShowInput(ProductionInputKind Kind, string Name, string? Path, bool Loop)
{
    public string? Label { get; init; }
    public AudioStripSettings? Strip { get; init; }
    public string? Scene { get; init; }
    public Dictionary<string, string>? Fields { get; init; }
    public GraphicsTimerMode? TimerMode { get; init; }
    public double? TimerSeconds { get; init; }
    public int ReplaySeconds { get; init; }
    public int? ReplayOf { get; init; }
}

/// <param name="Input">Index of the input on the channel.</param>
public sealed record ShowOverlay(int Input, KeySettings Key, NormalizedRect Destination, float Opacity);

/// <param name="IsoInput">ISO recordings: index of the input recorded.</param>
/// <param name="Url">Address. For streams the secret part (key, password) is
/// stored only in <see cref="ProtectedUrl"/> when the host can protect it, otherwise left out.</param>
public sealed record ShowOutput(string Name, OutputSettings Settings, int? IsoInput)
{
    /// <summary>The full address, protected for the current Windows user (DPAPI) — null when not saved.</summary>
    public string? ProtectedUrl { get; init; }
}

/// <summary>Protects stream keys in show files (e.g. Windows DPAPI for the current user). Null = keys are not saved.</summary>
public interface ISecretProtector
{
    string Protect(string secret);

    /// <exception cref="InvalidOperationException">The secret cannot be read here (other user or PC).</exception>
    string Unprotect(string protectedSecret);
}

/// <summary>Reads and writes show files.</summary>
public static class ShowFile
{
    public const string Extension = ".mixit";

    private static readonly JsonSerializerOptions Options = new(JsonSerializerDefaults.Web)
    {
        WriteIndented = true,
        Converters = { new JsonStringEnumConverter() },
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
        // Files for people to read (not HTML): keep "&", quotes and accents readable.
        Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
    };

    public static void Save(ShowDocument show, string path)
    {
        ArgumentNullException.ThrowIfNull(show);
        string? dir = System.IO.Path.GetDirectoryName(System.IO.Path.GetFullPath(path));
        if (dir is not null) Directory.CreateDirectory(dir);
        string temp = path + ".tmp";
        File.WriteAllText(temp, JsonSerializer.Serialize(show, Options));
        File.Move(temp, path, overwrite: true);  // never leave a half-written show
    }

    /// <exception cref="InvalidDataException">Not a MIX IT show file or a newer version.</exception>
    public static ShowDocument Load(string path)
    {
        if (new FileInfo(path).Length > 16 * 1024 * 1024) throw new InvalidDataException("Show file larger than 16 MB.");
        ShowDocument? show;
        try
        {
            show = JsonSerializer.Deserialize<ShowDocument>(File.ReadAllText(path), Options);
        }
        catch (JsonException ex)
        {
            throw new InvalidDataException($"Not a valid show file: {ex.Message}", ex);
        }
        if (show is null || show.Product != "MIX IT") throw new InvalidDataException("Not a MIX IT show file.");
        if (show.Version > ShowDocument.CurrentVersion)
            throw new InvalidDataException($"Show file version {show.Version} needs a newer MIX IT (this one reads {ShowDocument.CurrentVersion}).");
        return show;
    }
}

public sealed partial class ProductionService
{
    /// <summary>The current production as a show document (call while stopped or running).</summary>
    public ShowDocument ExportShow(ISecretProtector? protector)
    {
        var inputs = new List<ShowInput>();
        for (int i = 0; i < _specs.Count; i++)
        {
            InputSpec s = _specs[i];
            AudioStripSettings? strip = null;
            if (_session is not null && s.Kind != ProductionInputKind.Graphics && s.Kind != ProductionInputKind.Replay)
            {
                try
                {
                    strip = _session.GetAudioStrip(_inputs[i].Id);
                }
                catch (MixItNativeException)
                {
                    strip = s.Strip;
                }
            }
            inputs.Add(new ShowInput(s.Kind, s.Name, s.Path, s.Loop)
            {
                Label = s.Label,
                Strip = strip ?? s.Strip,
                Scene = s.Scene,
                Fields = s.Fields.Count > 0 ? new Dictionary<string, string>(s.Fields) : null,
                TimerMode = s.Timer?.Mode,
                TimerSeconds = s.Timer?.Value?.TotalSeconds,
                ReplaySeconds = s.ReplaySeconds,
                ReplayOf = s.ReplayOf is null ? null : _specs.IndexOf(s.ReplayOf),
            });
        }
        var outputs = _outputs.Select(o =>
        {
            bool network = o.Settings.IsNetwork;
            string redacted = UrlRedaction.Redact(o.Settings.Url);
            bool hasSecret = network && redacted != o.Settings.Url;
            return new ShowOutput(o.Name, o.Settings with { Url = hasSecret ? redacted : o.Settings.Url, SourceInputId = o.Settings.SourceInputId == OutputSettings.MultiviewSource ? OutputSettings.MultiviewSource : 0 },
                o.Source is null ? null : _specs.IndexOf(o.Source))
            {
                ProtectedUrl = hasSecret && protector is not null ? protector.Protect(o.Settings.Url) : null,
            };
        }).ToList();
        var buses = new Dictionary<AudioBus, AudioBusSettings>(_buses);
        return new ShowDocument
        {
            Format = new ShowFormat(_format.Name, _format.Width, _format.Height, _format.FrameRate.Numerator, _format.FrameRate.Denominator),
            Compositor = _compositor,
            Inputs = inputs,
            Overlays = _overlays.Select(o => o.Source is null ? null
                : new ShowOverlay(_specs.IndexOf(o.Source), o.Layer.Key, o.Layer.Destination, o.Layer.Opacity)).ToList(),
            PictureInPicture = _pipInputId is uint pip && _inputs.FindIndex(i => i.Id == pip) is int p and >= 0 ? p : null,
            Outputs = outputs,
            Buses = buses,
            MonitorSource = _monitorSource,
        };
    }

    /// <summary>
    /// Replaces the production with a show (not while running). Inputs that cannot
    /// be opened (missing file, unplugged device) are skipped; every problem is
    /// returned as a warning for the operator — nothing is dropped silently.
    /// </summary>
    public IReadOnlyList<string> ImportShow(ShowDocument show, ISecretProtector? protector)
    {
        ArgumentNullException.ThrowIfNull(show);
        RequireStopped("open a show");
        var warnings = new List<string>();
        foreach (int outputId in Outputs.Select(o => o.Id).ToList()) RemoveOutput(outputId);
        while (_inputs.Count > 0) RemoveInput(_inputs[^1].Id);
        for (int c = 0; c < _overlays.Length; c++) SetOverlay(c, null);
        SetPictureInPicture(null);

        if (show.Format.Width is < 16 or > 7680 || show.Format.Height is < 16 or > 4320 || show.Format.RateNumerator == 0 || show.Format.RateDenominator == 0)
            throw new InvalidDataException("The show file has an invalid format.");
        var format = new ProductionFormat(show.Format.Name, show.Format.Width, show.Format.Height,
            new MediaRate(show.Format.RateNumerator, show.Format.RateDenominator));
        SetFormat(ProductionFormat.Standard.FirstOrDefault(f => f == format) ?? format, show.Compositor);

        var ids = new uint?[show.Inputs.Count];
        for (int i = 0; i < show.Inputs.Count; i++)
        {
            ShowInput s = show.Inputs[i];
            try
            {
                ProductionInput? added = s.Kind switch
                {
                    ProductionInputKind.TestSignal => AddTestSignal(),
                    ProductionInputKind.MediaFile when s.Path is not null && File.Exists(s.Path) => AddMediaFile(s.Path, s.Loop),
                    ProductionInputKind.MediaFile => Warn($"{s.Name}: file not found ({s.Path})"),
                    ProductionInputKind.CaptureDevice => GetCaptureDevices().FirstOrDefault(d => d.Id == s.Path) is { } dev
                        ? AddCaptureDevice(dev) : Warn($"{s.Name}: capture device not connected"),
                    ProductionInputKind.AudioCapture => GetAudioDevices(AudioDeviceDirection.Capture).FirstOrDefault(d => d.Id == s.Path) is { } ad
                        ? AddAudioCapture(ad) : Warn($"{s.Name}: audio device not connected"),
                    ProductionInputKind.Graphics when s.Scene is not null => AddGraphics(s.Name, s.Scene, s.Fields),
                    ProductionInputKind.Replay when s.ReplayOf is int src && src >= 0 && src < i && ids[src] is uint srcId =>
                        AddReplay(srcId, Math.Clamp(show.Inputs[src].ReplaySeconds, 5, 600)),
                    _ => Warn($"{s.Name}: cannot be restored ({s.Kind})"),
                };
                if (added is null) continue;
                ids[i] = added.Id;
                string wanted = s.Label ?? s.Name;  // generated names (Test signal N, Replay …) may differ on reopen
                if (added.Name != wanted) RenameInput(added.Id, wanted);
                if (s.Strip is not null && s.Kind is not (ProductionInputKind.Graphics or ProductionInputKind.Replay)) SetAudioStrip(added.Id, s.Strip);
                if (s.TimerMode is GraphicsTimerMode mode)
                    SetGraphicsTimer(added.Id, mode, s.TimerSeconds is double sec ? TimeSpan.FromSeconds(sec) : null, false);
            }
            catch (Exception ex) when (ex is MixItNativeException or ArgumentException or InvalidOperationException or IOException)
            {
                warnings.Add($"{s.Name}: {ex.Message}");
            }
        }

        for (int c = 0; c < Math.Min(show.Overlays.Count, _overlays.Length); c++)
        {
            if (show.Overlays[c] is not { } o) continue;
            if (o.Input < 0 || o.Input >= ids.Length || ids[o.Input] is not uint oid)
            {
                warnings.Add($"Overlay {c + 1}: its input was not restored");
                continue;
            }
            TryApply(warnings, $"Overlay {c + 1}", () => SetOverlay(c, oid, o.Key, o.Destination, o.Opacity));
        }
        if (show.PictureInPicture is int pipIndex && pipIndex >= 0 && pipIndex < ids.Length && ids[pipIndex] is uint pipId)
            TryApply(warnings, "Picture-in-picture", () => SetPictureInPicture(pipId));
        foreach ((AudioBus bus, AudioBusSettings settings) in show.Buses)
            TryApply(warnings, $"Bus {bus}", () => SetAudioBus(bus, settings));
        TryApply(warnings, "Monitor", () => SetMonitorSource(show.MonitorSource));

        foreach (ShowOutput o in show.Outputs)
        {
            OutputSettings settings = o.Settings;
            if (o.ProtectedUrl is not null)
            {
                try
                {
                    settings = settings with { Url = protector?.Unprotect(o.ProtectedUrl) ?? throw new InvalidOperationException("no key store") };
                }
                catch (InvalidOperationException)
                {
                    warnings.Add($"{o.Name}: the stream key could not be read on this account — enter it again");
                }
            }
            else if (settings.IsNetwork && settings.Url.Contains("***", StringComparison.Ordinal))
            {
                warnings.Add($"{o.Name}: the stream key was not saved — enter it again");
            }
            uint? iso = null;
            if (o.IsoInput is int isoIndex)
            {
                if (isoIndex < 0 || isoIndex >= ids.Length || ids[isoIndex] is not uint isoId)
                {
                    warnings.Add($"{o.Name}: its ISO input was not restored");
                    continue;
                }
                iso = isoId;
            }
            TryApply(warnings, o.Name, () => AddOutput(o.Name, settings, iso));
        }
        RaiseChanged();
        return warnings;

        ProductionInput? Warn(string message)
        {
            warnings.Add(message);
            return null;
        }
    }

    private static void TryApply(List<string> warnings, string what, Action apply)
    {
        try
        {
            apply();
        }
        catch (Exception ex) when (ex is MixItNativeException or ArgumentException or InvalidOperationException)
        {
            warnings.Add($"{what}: {ex.Message}");
        }
    }
}
