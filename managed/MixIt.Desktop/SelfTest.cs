using System.Windows;
using Microsoft.Extensions.Logging;
using MixIt.Core.Production;
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
        ProductionService production, ILogger logger, Application app)
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
            mixer.IsActive = false;

            await vm.ToggleProductionAsync();
            if (vm.IsRunning) failures.Add("production did not stop");
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
