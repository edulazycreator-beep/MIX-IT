using System.Text.Json;

namespace MixIt.Core.Graphics;

/// <summary>A ready-made graphics scene and the fields an operator fills in.</summary>
/// <param name="Id">Stable identifier (saved with productions).</param>
/// <param name="Name">Shown to the operator.</param>
/// <param name="Fields">Placeholders in the scene, with default values, in display order.</param>
/// <param name="UsesTimer">Shows the input's stopwatch/countdown.</param>
public sealed record GraphicsTemplate(string Id, string Name, IReadOnlyList<KeyValuePair<string, string>> Fields,
    bool UsesTimer, string SceneJson);

/// <summary>
/// Built-in MIX IT title templates (original designs): lower third, title,
/// clock, countdown, scoreboard, logo bug. Scenes follow the engine's JSON
/// scene format (docs/API_CONTRACT.md, graphics inputs).
/// </summary>
public static class GraphicsTemplates
{
    private const string Accent = "#FFE8A33D";  // MIX IT amber
    private const string Panel = "#E6101418";

    public static IReadOnlyList<GraphicsTemplate> All { get; } =
    [
        LowerThird(),
        Title(),
        Clock(),
        Countdown(),
        Scoreboard(),
    ];

    public static GraphicsTemplate LowerThird() => Make("lower-third", "Lower third",
        [new("name", "Name"), new("title", "Title / role")], false,
        Rect(0.05, 0.74, 0.012, 0.14, Accent),
        Rect(0.062, 0.74, 0.5, 0.09, Panel),
        Rect(0.062, 0.83, 0.42, 0.05, "#E62A2F36"),
        Text(0.08, 0.745, 0.47, 0.08, "{name}", 0.055, "#FFFFFF", bold: true),
        Text(0.08, 0.832, 0.39, 0.046, "{title}", 0.032, "#FFE6E6E6"));

    public static GraphicsTemplate Title() => Make("title", "Full title",
        [new("headline", "Headline"), new("subtitle", "Subtitle")], false,
        Rect(0.0, 0.36, 1.0, 0.28, "#B0000000"),
        Rect(0.42, 0.535, 0.16, 0.006, Accent),
        Text(0.05, 0.38, 0.9, 0.15, "{headline}", 0.1, "#FFFFFF", bold: true, align: "center", shadow: true),
        Text(0.05, 0.55, 0.9, 0.07, "{subtitle}", 0.045, "#FFE0E0E0", align: "center"));

    public static GraphicsTemplate Clock() => Make("clock", "Clock (corner)", [], false,
        Rect(0.82, 0.04, 0.15, 0.065, Panel, radius: 0.008),
        Text(0.82, 0.04, 0.15, 0.065, "{time}", 0.042, "#FFFFFF", bold: true, align: "center"));

    public static GraphicsTemplate Countdown() => Make("countdown", "Countdown", [new("label", "Starting in")], true,
        Rect(0.3, 0.32, 0.4, 0.36, Panel, radius: 0.02),
        Text(0.3, 0.35, 0.4, 0.08, "{label}", 0.05, "#FFE0E0E0", align: "center"),
        Text(0.3, 0.43, 0.4, 0.22, "{timer}", 0.18, Accent, bold: true, align: "center"));

    public static GraphicsTemplate Scoreboard() => Make("scoreboard", "Scoreboard",
        [new("team_a", "HOME"), new("score_a", "0"), new("team_b", "AWAY"), new("score_b", "0"), new("period", "1")], true,
        Rect(0.03, 0.04, 0.42, 0.07, Panel, radius: 0.008),
        Text(0.04, 0.045, 0.12, 0.06, "{team_a}", 0.035, "#FFFFFF", bold: true),
        Rect(0.16, 0.045, 0.05, 0.06, Accent),
        Text(0.16, 0.045, 0.05, 0.06, "{score_a}", 0.04, "#FF101418", bold: true, align: "center"),
        Rect(0.215, 0.045, 0.05, 0.06, Accent),
        Text(0.215, 0.045, 0.05, 0.06, "{score_b}", 0.04, "#FF101418", bold: true, align: "center"),
        Text(0.27, 0.045, 0.1, 0.06, "{team_b}", 0.035, "#FFFFFF", bold: true),
        Text(0.37, 0.045, 0.075, 0.03, "P{period}", 0.022, "#FFC8C8C8", align: "center"),
        Text(0.37, 0.075, 0.075, 0.03, "{timer}", 0.024, "#FFFFFF", align: "center"));

    /// <summary>A logo/bug in a corner from an image file (PNG with transparency works best).</summary>
    public static GraphicsTemplate Logo(string imagePath) => Make("logo", "Logo", [], false,
        Image(0.84, 0.04, 0.13, 0.13, imagePath));

    public static GraphicsTemplate? Find(string id) => All.FirstOrDefault(t => t.Id == id);

    private static GraphicsTemplate Make(string id, string name, List<KeyValuePair<string, string>> fields, bool timer,
        params Dictionary<string, object>[] elements) =>
        new(id, name, fields, timer, JsonSerializer.Serialize(new Dictionary<string, object> { ["elements"] = elements }));

    private static Dictionary<string, object> Rect(double x, double y, double w, double h, string color, double radius = 0) =>
        new() { ["type"] = "rect", ["x"] = x, ["y"] = y, ["w"] = w, ["h"] = h, ["color"] = color, ["radius"] = radius };

    private static Dictionary<string, object> Text(double x, double y, double w, double h, string text, double size, string color,
        bool bold = false, string align = "left", bool shadow = false) =>
        new()
        {
            ["type"] = "text", ["x"] = x, ["y"] = y, ["w"] = w, ["h"] = h, ["text"] = text, ["size"] = size,
            ["color"] = color, ["bold"] = bold, ["align"] = align, ["shadow"] = shadow,
        };

    private static Dictionary<string, object> Image(double x, double y, double w, double h, string path) =>
        new() { ["type"] = "image", ["x"] = x, ["y"] = y, ["w"] = w, ["h"] = h, ["path"] = path };
}
