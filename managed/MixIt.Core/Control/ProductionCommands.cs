using System.Globalization;
using MixIt.Core.Production;
using MixIt.Interop;
using MixIt.Protocol;

namespace MixIt.Core.Control;

/// <summary>
/// The built-in production commands (switching, overlays, graphics, audio,
/// outputs, replay). Each maps onto <see cref="ProductionService"/> exactly as
/// the operator's buttons do.
/// </summary>
public static class ProductionCommands
{
    private static CommandParameter P(string name, ParameterKind kind, bool required, string description) =>
        new(name, kind, required, description);

    private static readonly CommandParameter InputArg = P("input", ParameterKind.Input, true, "Input number (1 = first), id:N or name");

    /// <summary>Resolves an input reference: its number (1 = first), "id:N" (engine id) or its name.</summary>
    public static ProductionInput ResolveInput(ProductionService production, string reference)
    {
        ArgumentNullException.ThrowIfNull(production);
        string r = reference.Trim();
        IReadOnlyList<ProductionInput> inputs = production.Inputs;
        if (r.StartsWith("id:", StringComparison.OrdinalIgnoreCase)
            && uint.TryParse(r.AsSpan(3), NumberStyles.None, CultureInfo.InvariantCulture, out uint id))
        {
            return inputs.FirstOrDefault(i => i.Id == id) ?? throw new CommandException($"No input with id {id}.");
        }
        if (int.TryParse(r, NumberStyles.None, CultureInfo.InvariantCulture, out int number))
        {
            return number >= 1 && number <= inputs.Count ? inputs[number - 1]
                : throw new CommandException($"No input {number} (there are {inputs.Count}).");
        }
        return inputs.FirstOrDefault(i => string.Equals(i.Name, r, StringComparison.OrdinalIgnoreCase))
            ?? throw new CommandException($"No input named '{r}'.");
    }

    public static ProductionOutput ResolveOutput(ProductionService production, string reference)
    {
        string r = reference.Trim();
        IReadOnlyList<ProductionOutput> outputs = production.Outputs;
        if (int.TryParse(r, NumberStyles.None, CultureInfo.InvariantCulture, out int id) && outputs.FirstOrDefault(o => o.Id == id) is { } byId)
            return byId;
        return outputs.FirstOrDefault(o => string.Equals(o.Name, r, StringComparison.OrdinalIgnoreCase))
            ?? throw new CommandException($"No output '{r}'.");
    }

    private static uint Input(ProductionService p, CommandArgs a, string name = "input") => ResolveInput(p, a.String(name)).Id;

    private static void RequireRunning(ProductionService p)
    {
        if (!p.IsRunning) throw new CommandException("Production is not running.");
    }

    public static void Register(CommandService commands, ProductionService production)
    {
        ArgumentNullException.ThrowIfNull(commands);
        ArgumentNullException.ThrowIfNull(production);
        ProductionService p = production;

        // ---- switching
        commands.Register(new("Cut", "Swap preview and program instantly.", []), _ =>
        {
            RequireRunning(p);
            p.Cut();
        });
        commands.Register(new("Auto", "Transition preview to program.",
            [P("kind", ParameterKind.String, false, "mix (default), fade, wipe or cut"), P("frames", ParameterKind.Integer, false, "Duration in frames (default 30)")]), a =>
        {
            RequireRunning(p);
            TransitionKind kind = a.String("kind", "mix").ToLowerInvariant() switch
            {
                "mix" => TransitionKind.Mix,
                "fade" => TransitionKind.Fade,
                "wipe" => TransitionKind.Wipe,
                "cut" => TransitionKind.Cut,
                var k => throw new CommandException($"Unknown transition '{k}'."),
            };
            if (!p.Auto(kind, (uint)a.Int("frames", 30, 1, 600))) throw new CommandException("A transition is already running.");
        });
        commands.Register(new("SetPreview", "Put an input on preview.", [InputArg]), a => p.SetPreview(Input(p, a)));
        commands.Register(new("SetProgram", "Put an input straight on program (cut).", [InputArg]), a => p.SetProgram(Input(p, a)));
        commands.Register(new("PictureInPicture", "Show an input as a picture-in-picture box, or none.",
            [P("input", ParameterKind.Input, false, "Input, or empty/none to remove the box")]), a =>
        {
            string r = a.String("input", "none");
            p.SetPictureInPicture(r.Equals("none", StringComparison.OrdinalIgnoreCase) ? null : ResolveInput(p, r).Id);
        });
        commands.Register(new("StartProduction", "Start the production (engine session).", []), async _ =>
        {
            await p.StartAsync().ConfigureAwait(true);
            return null;
        });
        commands.Register(new("StopProduction", "Stop the production (outputs are finalized).", []), async _ =>
        {
            await p.StopAsync().ConfigureAwait(true);
            return null;
        });

        // ---- overlays
        CommandParameter channel = P("channel", ParameterKind.Integer, true, "Overlay channel 1..4");
        CommandParameter fade = P("fade", ParameterKind.Integer, false, "Fade in frames (default 15; 0 = cut)");
        int Channel(CommandArgs a) => a.Int("channel", null, 1, NativeSession.OverlayChannels) - 1;
        commands.Register(new("OverlayIn", "Take an overlay channel on air.", [channel, fade], "Overlays"), a =>
            p.ShowOverlay(Channel(a), true, a.Int("fade", 15, 0, 600)));
        commands.Register(new("OverlayOut", "Take an overlay channel off air.", [channel, fade], "Overlays"), a =>
            p.ShowOverlay(Channel(a), false, a.Int("fade", 15, 0, 600)));
        commands.Register(new("OverlayToggle", "Toggle an overlay channel.", [channel, fade], "Overlays"), a =>
        {
            int c = Channel(a);
            p.ShowOverlay(c, !p.GetOverlay(c).State.Visible, a.Int("fade", 15, 0, 600));
        });
        commands.Register(new("OverlaySelect", "Choose the input of an overlay channel (keeps its key and placement).",
            [channel, P("input", ParameterKind.Input, false, "Input, or none to empty the channel")], "Overlays"), a =>
        {
            int c = Channel(a);
            string r = a.String("input", "none");
            (_, KeySettings key, _) = p.GetOverlay(c);
            p.SetOverlay(c, r.Equals("none", StringComparison.OrdinalIgnoreCase) ? null : ResolveInput(p, r).Id, key, p.GetOverlayDestination(c));
        });

        // ---- graphics
        commands.Register(new("SetText", "Change a graphics field on air.",
            [InputArg, P("field", ParameterKind.String, true, "Field name, e.g. name"), P("value", ParameterKind.String, false, "New text (empty clears)")], "Graphics"), a =>
            p.SetGraphicsField(Input(p, a), a.String("field"), a.Has("value") ? a.String("value") : ""));
        commands.Register(new("TimerStart", "Start a graphics timer.", [InputArg], "Graphics"), a =>
        {
            uint id = Input(p, a);
            p.SetGraphicsTimer(id, p.GetGraphicsInfo(id).TimerMode, null, true);
        });
        commands.Register(new("TimerPause", "Pause a graphics timer.", [InputArg], "Graphics"), a =>
        {
            uint id = Input(p, a);
            p.SetGraphicsTimer(id, p.GetGraphicsInfo(id).TimerMode, null, false);
        });
        commands.Register(new("TimerSet", "Set a graphics timer (paused).",
            [InputArg, P("mode", ParameterKind.String, false, "countdown (default) or stopwatch"), P("seconds", ParameterKind.Integer, false, "Start value (default 0)")], "Graphics"), a =>
        {
            GraphicsTimerMode mode = a.String("mode", "countdown").ToLowerInvariant() switch
            {
                "countdown" => GraphicsTimerMode.Countdown,
                "stopwatch" => GraphicsTimerMode.Stopwatch,
                var m => throw new CommandException($"Unknown timer mode '{m}'."),
            };
            p.SetGraphicsTimer(Input(p, a), mode, TimeSpan.FromSeconds(a.Int("seconds", 0, 0, 359_999)), false);
        });

        // ---- audio
        commands.Register(new("SetVolume", "Set an input's fader.",
            [InputArg, P("db", ParameterKind.Number, true, "Fader in dB (-96 = off .. +12)")], "Audio"), a =>
        {
            uint id = Input(p, a);
            p.SetAudioStrip(id, p.GetAudioStrip(id) with { FaderDb = (float)a.Number("db", null, -96, 12) });
        });
        commands.Register(new("Mute", "Mute, unmute or toggle an input.",
            [InputArg, P("value", ParameterKind.Boolean, false, "true, false or toggle (default)")], "Audio"), a =>
        {
            uint id = Input(p, a);
            AudioStripSettings s = p.GetAudioStrip(id);
            bool? value = a.Has("value") ? a.Bool("value") : null;  // default: toggle
            p.SetAudioStrip(id, s with { Mute = value ?? !s.Mute });
        });

        // ---- outputs
        CommandParameter output = P("output", ParameterKind.String, true, "Output id or name");
        commands.Register(new("StartOutput", "Start a recording or stream.", [output], "Outputs"), a =>
            p.StartOutput(ResolveOutput(p, a.String("output")).Id));
        commands.Register(new("StopOutput", "Stop a recording or stream (finalized in the background).", [output], "Outputs"), a =>
            p.StopOutput(ResolveOutput(p, a.String("output")).Id));
        commands.Register(new("StartRecordings", "Start every file output that is not running.", [], "Outputs"), _ =>
        {
            RequireRunning(p);
            foreach (ProductionOutput o in p.Outputs.Where(o => !o.Settings.IsNetwork))
            {
                if (p.GetOutputStatistics(o.Id)?.State is OutputState.Idle or OutputState.Failed or null) p.StartOutput(o.Id);
            }
        });
        commands.Register(new("StopRecordings", "Stop every file output.", [], "Outputs"), _ =>
        {
            foreach (ProductionOutput o in p.Outputs.Where(o => !o.Settings.IsNetwork)) p.StopOutput(o.Id);
        });

        // ---- replay
        CommandParameter replay = P("input", ParameterKind.Input, true, "Replay input");
        commands.Register(new("ReplayMark", "Mark the replay in-point (now).", [replay], "Replay"), a =>
            Task.FromResult<string?>(p.MarkReplayIn(Input(p, a)).ToString(CultureInfo.InvariantCulture)));
        commands.Register(new("ReplayPlay", "Play a replay from the mark (or the last seconds).",
            [replay, P("speed", ParameterKind.Number, false, "0.1..2 (default 0.5)"),
             P("seconds", ParameterKind.Number, false, "Without a mark (or with from=last): seconds before now (default 5)"),
             P("from", ParameterKind.String, false, "mark (default) or last")], "Replay"), a =>
        {
            bool fromMark = a.String("from", "mark").Equals("mark", StringComparison.OrdinalIgnoreCase);
            p.PlayReplay(Input(p, a), (float)a.Number("speed", 0.5, 0.1, 2.0), a.Number("seconds", 5, 0, 600), fromMark);
        });
        commands.Register(new("ReplayPause", "Pause or resume a replay.",
            [replay, P("value", ParameterKind.Boolean, false, "true (default), false or toggle")], "Replay"), a =>
        {
            uint id = Input(p, a);
            bool paused = a.Bool("value", true) ?? !p.GetReplayStatus(id).State.Paused;
            p.ControlReplay(id, null, paused);
        });
        commands.Register(new("ReplaySpeed", "Change a replay's speed.",
            [replay, P("speed", ParameterKind.Number, true, "0.1..2")], "Replay"), a =>
            p.ControlReplay(Input(p, a), (float)a.Number("speed", null, 0.1, 2.0), null));

        // ---- timing (macros)
        commands.Register(new("Wait", "Wait (in macros).", [P("ms", ParameterKind.Integer, true, "Milliseconds, up to 10 minutes")], "Macros"), async a =>
        {
            await Task.Delay(a.Int("ms", null, 0, 600_000)).ConfigureAwait(true);
            return null;
        });
    }

    /// <summary>Snapshot for remote clients (call on the production thread).</summary>
    public static ProductionState Snapshot(ProductionService p, IReadOnlyList<TallyEntry> tally)
    {
        ArgumentNullException.ThrowIfNull(p);
        SessionStatistics? s = p.IsRunning ? p.GetStatistics() : null;
        var inputs = p.Inputs.Select((i, n) => new InputInfo(i.Id, n + 1, i.Name, i.Kind.ToString())).ToList();
        var outputs = p.Outputs.Select(o =>
        {
            string state = "Idle";
            try
            {
                state = p.GetOutputStatistics(o.Id)?.State.ToString() ?? "Idle";
            }
            catch (MixItNativeException)
            {
                state = "Unknown";
            }
            string kind = o.SourceInputId is not null ? "Iso" : o.IsMultiview ? "Multiview" : o.Settings.IsNetwork ? "Stream" : "Recording";
            return new OutputInfo(o.Id, o.Name, kind, state, UrlRedaction.Redact(o.Settings.Url));
        }).ToList();
        return new ProductionState(p.EngineAvailable, p.IsRunning, p.Format.Name, s?.ProgramInputId ?? 0, s?.PreviewInputId ?? 0,
            inputs, outputs, tally);
    }
}
