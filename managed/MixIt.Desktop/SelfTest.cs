using System.IO;
using System.Windows;
using Microsoft.Extensions.Logging;
using System.Net;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Net.Sockets;
using MixIt.Core.Control;
using MixIt.Core.Production;
using MixIt.Desktop.Services;
using MixIt.Desktop.ViewModels;
using MixIt.Interop;

namespace MixIt.Desktop;

/// <summary>
/// "MixIt.exe --self-test production": drives the Production Studio view
/// model exactly as the buttons do (inputs, start, monitors, CUT, AUTO, PiP,
/// audio meters and a fader, stop), checks the result against the engine statistics, writes one
/// "Self-test production: PASS|FAIL ..." line to the log and exits with 0 or 1.
/// </summary>
internal static class SelfTest
{
    public static async Task RunProductionAsync(Task engineStart, ProductionViewModel vm, AudioMixerViewModel mixer,
        OutputsViewModel outputs, GraphicsViewModel graphics, ReplayViewModel replay, ControlHost control, Task controlStart,
        SettingsViewModel settings, StatusBarViewModel statusBar, ProductionService production, ILogger logger, Application app)
    {
        var failures = new List<string>();
        try
        {
            await engineStart;
            if (!production.EngineAvailable) throw new InvalidOperationException("engine did not start");

            vm.SelectedFormat = ProductionFormat.Standard.First(f => f is { Width: 1280, FrameRate.Numerator: 30 });
            vm.AddTestSignalCommand.Execute(null);
            vm.AddTestSignalCommand.Execute(null);
            if (vm.Inputs.Count != 2) failures.Add($"expected 2 input tiles, got {vm.Inputs.Count}");

            await vm.ToggleProductionAsync();
            if (!vm.IsRunning) throw new InvalidOperationException($"production did not start: {vm.Error}");
            await Task.Delay(1500);
            if (vm.Program.FramesShown < 10) failures.Add($"program monitor showed {vm.Program.FramesShown} frames");
            if (vm.Preview.FramesShown < 10) failures.Add($"preview monitor showed {vm.Preview.FramesShown} frames");

            vm.CutCommand.Execute(null);
            await Task.Delay(300);
            vm.SelectedTransition = TransitionKind.Mix;
            vm.DurationFrames = 15;
            vm.AutoCommand.Execute(null);
            await Task.Delay(1200);
            vm.SelectedPip = vm.PipChoices.First(c => c.InputId is not null);
            await Task.Delay(500);

            SessionStatistics s = production.GetStatistics() ?? throw new InvalidOperationException("no statistics");
            if (s.TransitionsCompleted != 2) failures.Add($"{s.TransitionsCompleted} transitions completed, expected 2");
            if (s.ProgramInputId != vm.Inputs[0].Id) failures.Add("program is not back on input 1 after CUT + AUTO");
            if (s.CompositedFrames == 0) failures.Add("nothing was composited (PiP / transition)");
            if (!vm.Inputs[0].OnProgram || !vm.Inputs[1].OnPreview) failures.Add("tally does not match the buses");
            if (vm.Error is not null) failures.Add($"operator error shown: {vm.Error}");

            // Audio: the program input's line-up tone (-20 dBFS) reaches master;
            // a fader move in the mixer reaches the engine.
            mixer.IsActive = true;
            await Task.Delay(400);
            BusViewModel master = mixer.Buses[0];
            // Input 1 on program plus input 2 as PiP: one or two -20 dBFS tones (-20..-14 dBFS).
            if (master.PeakLeft < -26 || master.PeakLeft > -8) failures.Add($"master meter shows {master.PeakLeft:0.0} dBFS, expected -20..-14");
            if (mixer.Strips.Count != 2) failures.Add($"mixer shows {mixer.Strips.Count} strips, expected 2");
            else
            {
                mixer.Strips[0].FaderDb = -10;
                if (production.GetAudioStrip(mixer.Strips[0].Id).FaderDb != -10f) failures.Add("fader move did not reach the engine");
            }
            if (mixer.Error is not null) failures.Add($"mixer error shown: {mixer.Error}");
            // Plug-in scan of the default VST3 folders must complete (the folders may be empty).
            await mixer.ScanPluginsAsyncForTest();
            if (mixer.PluginStatus.StartsWith("Scan failed", StringComparison.Ordinal)) failures.Add(mixer.PluginStatus);
            if (mixer.Strips.Count > 0 && mixer.Strips[0].Inserts.Count != NativeSession.AudioInsertSlots)
            {
                failures.Add("strip does not show the insert slots");
            }
            mixer.IsActive = false;

            // Recording through the Streaming & Recording section: add, start, stop, finalized file.
            outputs.IsActive = true;
            await outputs.CheckEncodersCommand.ExecuteAsync();
            if (!outputs.EncoderStatus.StartsWith("Automatic uses", StringComparison.Ordinal)) failures.Add($"encoders: {outputs.EncoderStatus}");
            string recDir = Path.Combine(Path.GetTempPath(), "mixit_selftest_rec");
            if (Directory.Exists(recDir)) Directory.Delete(recDir, true);
            outputs.Address = Path.Combine(recDir, "selftest.mp4");
            outputs.VideoKbps = 2000;
            outputs.AddCommand.Execute(null);
            OutputRowViewModel? row = outputs.Outputs.LastOrDefault();
            if (row is null || outputs.Error is not null)
            {
                failures.Add($"could not add a recording: {outputs.Error}");
            }
            else
            {
                row.StartCommand.Execute(null);
                await Task.Delay(2500);
                OutputStatistics? rec = production.GetOutputStatistics(production.Outputs[^1].Id);
                if (rec?.State != OutputState.Running) failures.Add($"recording state {rec?.State}: {rec?.LastError}");
                row.StopCommand.Execute(null);
                for (int i = 0; i < 100 && production.GetOutputStatistics(production.Outputs[^1].Id)?.State != OutputState.Idle; i++)
                {
                    await Task.Delay(50);
                }
                rec = production.GetOutputStatistics(production.Outputs[^1].Id);
                string file = Path.Combine(recDir, "selftest.mp4");
                if (rec?.State != OutputState.Idle) failures.Add($"recording did not finish: {rec?.State}");
                else if (!File.Exists(file) || new FileInfo(file).Length < 20_000) failures.Add("recorded file missing or too small");
                else logger.LogInformation("Self-test recording: {Encoder}, {Bytes} bytes, {Duration}", rec.Encoder, rec.BytesWritten, rec.Duration);
                if (rec?.DroppedPackets > 0 || rec?.DataLost == true) failures.Add("recording dropped data");
            }
            if (outputs.Error is not null) failures.Add($"outputs error shown: {outputs.Error}");
            outputs.IsActive = false;

            await vm.ToggleProductionAsync();
            if (vm.IsRunning) failures.Add("production did not stop");

            // Graphics Studio: a lower third with a live field on overlay 1.
            graphics.IsActive = true;
            graphics.Template = graphics.Templates.First(t => t.Id == "lower-third");
            graphics.AddTemplateCommand.Execute(null);
            GraphicsItemViewModel? title = graphics.Items.LastOrDefault();
            if (title is null || graphics.Error is not null)
            {
                failures.Add($"could not add a lower third: {graphics.Error}");
            }
            else
            {
                title.Fields.First(f => f.Name == "name").Value = "Self-test";
                OverlayChannelViewModel ch = graphics.Channels[0];
                ch.Input = ch.Inputs.First(i => i.InputId == title.Id);
                await vm.ToggleProductionAsync();
                ch.CutCommand.Execute(null);
                await Task.Delay(500);
                (uint? onChannel, _, OverlayState overlay) = production.GetOverlay(0);
                GraphicsInfo gi = production.GetGraphicsInfo(title.Id);
                if (onChannel != title.Id || overlay.Level < 1f) failures.Add($"overlay 1 not on air (level {overlay.Level})");
                if (gi.FramesRendered < 1) failures.Add("the lower third was never rendered");
                if (gi.Warnings.Length > 0) failures.Add($"graphics warnings: {gi.Warnings}");
                if (production.GetGraphicsFields(title.Id)["name"] != "Self-test") failures.Add("field edit did not reach the engine");
                logger.LogInformation("Self-test graphics: overlay level {Level}, {Frames} frames rendered", overlay.Level, gi.FramesRendered);
                await vm.ToggleProductionAsync();
            }
            if (graphics.Error is not null) failures.Add($"graphics error shown: {graphics.Error}");
            graphics.IsActive = false;

            // Replay & Multiview: the multiview monitor runs; a replay of input 1
            // records, is marked and plays back at 50 %.
            replay.IsActive = true;
            replay.Source = replay.Sources.First(s => s.InputId == vm.Inputs[0].Id);
            replay.Seconds = 15;
            replay.AddReplayCommand.Execute(null);
            ReplayItemViewModel? item = replay.Items.LastOrDefault();
            if (item is null || replay.Error is not null)
            {
                failures.Add($"could not add a replay: {replay.Error}");
            }
            else
            {
                await vm.ToggleProductionAsync();
                await Task.Delay(1500);
                item.MarkCommand.Execute(null);
                await Task.Delay(300);
                item.PlayMarkCommand.Execute(null);
                await Task.Delay(700);
                ReplayStatus rs = production.GetReplayStatus(item.Id);
                if (replay.Multiview.FramesShown < 10) failures.Add($"multiview monitor showed {replay.Multiview.FramesShown} frames");
                if (rs.Buffer.Frames < 20) failures.Add($"replay buffer holds {rs.Buffer.Frames} frames");
                if (!rs.State.Active || Math.Abs(rs.State.Speed - 0.5f) > 1e-3) failures.Add("replay is not playing at 50 %");
                logger.LogInformation("Self-test replay: multiview {Frames} frames, buffer {BufferFrames} frames / {Bytes} bytes",
                    replay.Multiview.FramesShown, rs.Buffer.Frames, rs.Buffer.Bytes);
                await vm.ToggleProductionAsync();
            }
            if (replay.Error is not null) failures.Add($"replay error shown: {replay.Error}");
            replay.IsActive = false;

            // Automation & Control: a macro switches the program; the remote API
            // answers with the token only and runs a command; tally follows.
            await controlStart;
            control.Macros.Save(Macro.Parse("SelfTest", "SetPreview input=2\nCut"));
            await vm.ToggleProductionAsync();
            await Task.Delay(300);
            MacroRun run = await control.Macros.RunAsync("SelfTest");
            if (!run.Ok) failures.Add($"macro failed: {run.Error}");
            else if (production.GetStatistics()?.ProgramInputId != production.Inputs[1].Id) failures.Add("macro did not put input 2 on program");
            int port;
            using (var probe = new TcpListener(IPAddress.Loopback, 0))
            {
                probe.Start();
                port = ((IPEndPoint)probe.LocalEndpoint).Port;
                probe.Stop();
            }
            string token = MixIt.Remote.RemoteControlServer.NewToken();
            await control.Remote.StartAsync(new RemoteSettings { Enabled = true, Port = port, Token = token });
            using (var http = new HttpClient { BaseAddress = new Uri($"http://127.0.0.1:{port}"), Timeout = TimeSpan.FromSeconds(10) })
            {
                HttpStatusCode denied = (await http.GetAsync("/api/v1/state")).StatusCode;
                if (denied != HttpStatusCode.Unauthorized) failures.Add($"remote API without token answered {denied}");
                http.DefaultRequestHeaders.Authorization = new AuthenticationHeaderValue("Bearer", token);
                string state = await http.GetStringAsync("/api/v1/state");
                if (!state.Contains("\"running\":true", StringComparison.Ordinal)) failures.Add("remote state does not show the running production");
                HttpResponseMessage cut = await http.PostAsync("/api/v1/commands/Cut", new StringContent("{}", System.Text.Encoding.UTF8, "application/json"));
                if (!cut.IsSuccessStatusCode) failures.Add($"remote Cut answered {cut.StatusCode}");
            }
            await Task.Delay(300);  // the tally timer refreshes 10x/s
            uint program = production.GetStatistics()?.ProgramInputId ?? 0;
            if (!control.Tally.Current.Any(t => t.InputId == program && t.Program)) failures.Add("tally does not show the program input");
            logger.LogInformation("Self-test control: macro {Steps} steps, remote port {Port}, {Commands} commands, tally {Tally}",
                run.StepsRun, port, control.Commands.Commands.Count, string.Join(",", control.Tally.Current.Select(t => t.Program ? "P" : t.Preview ? "V" : "-")));
            await control.Remote.StopAsync();
            await vm.ToggleProductionAsync();

            // Settings & workspace: the show survives save + open; a shortcut switches; the dashboard shows ON AIR.
            string showPath = Path.Combine(Path.GetTempPath(), "mixit_selftest_show.mixit");
            int inputCount = production.Inputs.Count;
            settings.SaveShowFile(showPath);
            IReadOnlyList<string> showWarnings = settings.OpenShowFile(showPath);
            if (showWarnings.Count > 0) failures.Add($"show reopened with problems: {string.Join("; ", showWarnings)}");
            if (production.Inputs.Count != inputCount) failures.Add($"show reopened with {production.Inputs.Count} inputs, saved {inputCount}");
            await vm.ToggleProductionAsync();
            await Task.Delay(500);
            settings.TryRunShortcut("D2");
            await Task.Delay(200);
            uint beforeKey = production.GetStatistics()?.ProgramInputId ?? 0;
            if (!settings.TryRunShortcut("Space")) failures.Add("Space is not bound to a command");
            await Task.Delay(400);
            uint afterKey = production.GetStatistics()?.ProgramInputId ?? 0;
            if (afterKey == beforeKey) failures.Add("the Space shortcut did not cut");
            await Task.Delay(1100);
            if (!statusBar.IsOnAir) failures.Add("dashboard does not show ON AIR");
            logger.LogInformation("Self-test workspace: show {Inputs} inputs, shortcut program {Before}->{After}, dashboard '{OnAir}' {Resources}",
                inputCount, beforeKey, afterKey, statusBar.OnAir, statusBar.Resources);
            if (settings.Error is not null) failures.Add($"settings error shown: {settings.Error}");
            await vm.ToggleProductionAsync();

            logger.LogInformation(
                "Self-test production details: monitors {Program}/{Preview} frames, ticks {Ticks}, late {Late}, composited {Composited}, compositor {Compositor}",
                vm.Program.FramesShown, vm.Preview.FramesShown, s.Ticks, s.LateTicks, s.CompositedFrames, s.Compositor);
        }
        catch (Exception ex)
        {
            failures.Add($"{ex.GetType().Name}: {ex.Message}");
        }

        if (failures.Count == 0)
        {
            logger.LogInformation("Self-test production: PASS");
        }
        else
        {
            logger.LogError("Self-test production: FAIL — {Failures}", string.Join("; ", failures));
        }
        app.Shutdown(failures.Count == 0 ? 0 : 1);
    }
}
