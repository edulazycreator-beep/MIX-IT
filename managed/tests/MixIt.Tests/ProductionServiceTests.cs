using System.Runtime.InteropServices;
using Microsoft.Extensions.Logging.Abstractions;
using MixIt.Core.Engine;
using MixIt.Core.Graphics;
using MixIt.Core.Production;
using MixIt.Interop;
using Xunit;

namespace MixIt.Tests;

/// <summary>Production service against the real engine (short real-time runs).</summary>
public class ProductionServiceTests
{
    private static readonly ProductionFormat Small = new("test 640x360", 640, 360, MediaRate.Fps30);

    private static (EngineHost Host, ProductionService Service) Create()
    {
        var host = new EngineHost(new EngineHostOptions(), NullLogger<EngineHost>.Instance);
        host.Start();
        var service = new ProductionService(host, NullLogger<ProductionService>.Instance);
        service.SetFormat(Small);
        return (host, service);
    }

    [Fact]
    public async Task RunsAProductionWithPreviewProgramTransitionsAndPip()
    {
        (EngineHost host, ProductionService service) = Create();
        using (host)
        using (service)
        {
            int changes = 0;
            service.Changed += (_, _) => changes++;
            ProductionInput a = service.AddTestSignal();
            ProductionInput b = service.AddTestSignal();
            Assert.Equal("Test signal 2", b.Name);
            Assert.True(changes >= 2);

            await service.StartAsync();
            Assert.True(service.IsRunning);
            Assert.False(service.CanEditInputs);
            Assert.Throws<InvalidOperationException>(() => service.AddTestSignal());

            SessionStatistics? s = service.GetStatistics();
            Assert.NotNull(s);
            Assert.Equal(a.Id, s.Value.ProgramInputId);
            Assert.Equal(b.Id, s.Value.PreviewInputId);

            // Monitor copy into unmanaged memory, as the WPF monitors do.
            int length = 640 * 360 * 4;
            nint buffer = Marshal.AllocHGlobal(length);
            try
            {
                ProgramFrameInfo? frame = null;
                for (int i = 0; i < 25 && frame is null; i++)
                {
                    await Task.Delay(40);
                    frame = service.TryCopyBusFrame(VideoBus.Preview, buffer, length, 640 * 4);
                }
                Assert.NotNull(frame);
                Assert.Equal(b.Id, frame.Value.InputId);
            }
            finally
            {
                Marshal.FreeHGlobal(buffer);
            }

            service.SetPictureInPicture(b.Id);
            Assert.Equal(b.Id, service.PictureInPictureInputId);
            service.Cut();
            Assert.True(service.Auto(TransitionKind.Wipe, 10));
            Assert.False(service.Auto(TransitionKind.Mix, 10));  // one at a time
            await Task.Delay(700);
            s = service.GetStatistics();
            Assert.Equal(a.Id, s!.Value.ProgramInputId);
            Assert.Equal(2UL, s.Value.TransitionsCompleted);
            Assert.True(s.Value.CompositedFrames > 0);

            await service.StopAsync();
            Assert.False(service.IsRunning);
            Assert.Null(service.TryCopyBusFrame(VideoBus.Program, 0, 0, 0));
            service.RemoveInput(a.Id);
            Assert.Single(service.Inputs);
        }
    }

    [Fact]
    public async Task SurvivesAnEngineRestartAndFormatChange()
    {
        (EngineHost host, ProductionService service) = Create();
        using (host)
        using (service)
        {
            service.AddTestSignal();
            service.AddTestSignal();
            await service.StartAsync();

            host.Stop();  // closes the session underneath the service
            service.HandleEngineStateChanged(host.State);
            Assert.False(service.IsRunning);
            host.Start();

            service.SetFormat(ProductionFormat.Standard.First(f => f.Width == 1280));
            Assert.Equal(2, service.Inputs.Count);
            await service.StartAsync();  // rebuilds the session and its inputs
            await Task.Delay(200);
            SessionStatistics s = service.GetStatistics()!.Value;
            Assert.Equal(SessionState.Running, s.State);
            Assert.Equal(2, s.InputCount);
            Assert.Equal(service.Inputs[0].Id, s.ProgramInputId);
            await service.StopAsync();
        }
    }

    [Fact]
    public void MissingMediaFilesAreReportedAndNotAdded()
    {
        (EngineHost host, ProductionService service) = Create();
        using (host)
        using (service)
        {
            var ex = Assert.Throws<MixItNativeException>(() => service.AddMediaFile("/no/such/file.mp4"));
            Assert.Equal(NativeResult.NotFound, ex.Result);
            Assert.Empty(service.Inputs);
        }
    }

    [Fact]
    public void CaptureDeviceListingNeverThrows()
    {
        (EngineHost host, ProductionService service) = Create();
        using (host)
        using (service)
        {
            IReadOnlyList<CaptureDevice> devices = service.GetCaptureDevices();  // CI: none; Linux: unsupported
            Assert.All(devices, d => Assert.False(string.IsNullOrEmpty(d.Id)));
            if (!OperatingSystem.IsWindows()) Assert.Empty(devices);
        }
    }

    [Fact]
    public void AudioSettingsSurviveAFormatChange()
    {
        (EngineHost host, ProductionService service) = Create();
        using (host)
        using (service)
        {
            ProductionInput a = service.AddTestSignal();
            AudioStripSettings strip = service.GetAudioStrip(a.Id) with { FaderDb = -12f, Mute = true, BusMask = 0b11 };
            service.SetAudioStrip(a.Id, strip);
            service.SetAudioBus(AudioBus.AuxA, new AudioBusSettings { FaderDb = -4f });
            service.SetMonitorSource(AudioBus.AuxA);

            service.SetFormat(ProductionFormat.Standard.First(f => f.Width == 1280));  // rebuilds the session
            uint id = service.Inputs[0].Id;
            AudioStripSettings back = service.GetAudioStrip(id);
            Assert.Equal(-12f, back.FaderDb);
            Assert.True(back.Mute);
            Assert.True(back.SendsTo(AudioBus.AuxA));
            Assert.Equal(-4f, service.GetAudioBus(AudioBus.AuxA).FaderDb);
            Assert.Equal(AudioBus.AuxA, service.MonitorSource);
            Assert.Null(service.GetInputMeter(999));
        }
    }

    [Fact]
    public async Task RecordsTheProgramAndAnIsoAndKeepsOutputsAcrossRebuilds()
    {
        (EngineHost host, ProductionService service) = Create();
        using (host)
        using (service)
        {
            IReadOnlyList<VideoEncoderInfo> encoders = await service.QueryEncodersAsync();
            if (!encoders.Any(e => e.Usable)) return;  // engine without FFmpeg / H.264 encoder
            string dir = Path.Combine(Path.GetTempPath(), "mixit_managed_outputs");
            if (Directory.Exists(dir)) Directory.Delete(dir, true);
            Directory.CreateDirectory(dir);
            ProductionInput a = service.AddTestSignal();
            ProductionInput b = service.AddTestSignal();
            ProductionOutput program = service.AddOutput("Program", new OutputSettings { Url = Path.Combine(dir, "program.mkv"), VideoKbps = 1500 });
            ProductionOutput iso = service.AddOutput("ISO B", new OutputSettings { Url = Path.Combine(dir, "iso.mp4"), VideoKbps = 1000 }, b.Id);
            Assert.Null(program.SourceInputId);
            Assert.Equal(b.Id, iso.SourceInputId);
            Assert.Throws<InvalidOperationException>(() => service.StartOutput(program.Id));  // production stopped

            service.SetFormat(ProductionFormat.Standard.First(f => f.Width == 1280));  // rebuild: outputs come back
            Assert.Equal(2, service.Outputs.Count);
            Assert.Equal(service.Inputs[1].Id, service.Outputs[1].SourceInputId);

            await service.StartAsync();
            service.StartOutput(program.Id);
            service.StartOutput(iso.Id);
            await Task.Delay(2500);
            OutputStatistics running = service.GetOutputStatistics(program.Id)!;
            Assert.Equal(OutputState.Running, running.State);
            Assert.True(running.VideoPackets > 30);
            service.StopOutput(program.Id);
            OutputStatistics? done = null;
            for (int i = 0; i < 100; i++)
            {
                done = service.GetOutputStatistics(program.Id);
                if (done!.State == OutputState.Idle) break;
                await Task.Delay(50);
            }
            Assert.Equal(OutputState.Idle, done!.State);
            Assert.False(done.DataLost);
            await service.StopAsync();  // stops and finalizes the ISO recording
            Assert.Equal(OutputState.Idle, service.GetOutputStatistics(iso.Id)!.State);
            Assert.True(new FileInfo(Path.Combine(dir, "program.mkv")).Length > 10_000);
            Assert.True(new FileInfo(Path.Combine(dir, "iso.mp4")).Length > 10_000);

            service.RemoveInput(service.Inputs[1].Id);  // its ISO output goes with it
            Assert.Single(service.Outputs);
            service.RemoveOutput(program.Id);
            Assert.Empty(service.Outputs);
        }
    }

    [Fact]
    public async Task GraphicsOnAnOverlayChannelSurviveARebuild()
    {
        (EngineHost host, ProductionService service) = Create();
        using (host)
        using (service)
        {
            ProductionInput cam = service.AddTestSignal();
            GraphicsTemplate lt = GraphicsTemplates.LowerThird();
            ProductionInput g = service.AddGraphics("Lower third", lt.SceneJson, lt.Fields);
            Assert.Equal(ProductionInputKind.Graphics, g.Kind);
            service.SetGraphicsField(g.Id, "name", "Siti Rahma");
            service.SetOverlay(0, g.Id);
            service.ShowOverlay(0, true);
            Assert.Throws<ArgumentException>(() => service.SetGraphicsField(cam.Id, "name", "x"));

            service.SetFormat(ProductionFormat.Standard.First(f => f.Width == 1280));  // rebuild
            uint gid = service.Inputs[1].Id;
            Assert.Equal("Siti Rahma", service.GetGraphicsFields(gid)["name"]);
            (uint? onChannel, _, OverlayState state) = service.GetOverlay(0);
            Assert.Equal(gid, onChannel);
            Assert.True(state.Visible);
            Assert.Equal(1.0f, state.Level);

            service.SetGraphicsTimer(gid, GraphicsTimerMode.Countdown, TimeSpan.FromSeconds(30), false);
            await service.StartAsync();
            await Task.Delay(300);
            GraphicsInfo info = service.GetGraphicsInfo(gid);
            Assert.True(info.FramesRendered >= 1);
            Assert.Equal("", info.Warnings);
            Assert.Equal(TimeSpan.FromSeconds(30), info.Timer);
            await service.StopAsync();

            service.RemoveInput(gid);  // the overlay channel empties with it
            Assert.Null(service.GetOverlay(0).InputId);
            foreach (GraphicsTemplate t in GraphicsTemplates.All) Assert.False(string.IsNullOrEmpty(t.SceneJson));
        }
    }

    [Fact]
    public async Task ReplaysAnInputAndRecordsTheMultiview()
    {
        (EngineHost host, ProductionService service) = Create();
        using (host)
        using (service)
        {
            IReadOnlyList<VideoEncoderInfo> encoders = await service.QueryEncodersAsync();
            if (!encoders.Any(e => e.Usable)) return;  // engine without FFmpeg
            ProductionInput cam = service.AddTestSignal();
            ProductionInput replay = service.AddReplay(cam.Id, 20);
            Assert.Equal(ProductionInputKind.Replay, replay.Kind);
            Assert.Equal("Replay, last 20 s", replay.Detail);
            Assert.Throws<InvalidOperationException>(() => service.AddReplay(replay.Id));
            service.RenameInput(cam.Id, "Camera 1");
            Assert.Equal("Camera 1", service.Inputs[0].Name);

            service.SetFormat(ProductionFormat.Standard.First(f => f.Width == 1280));  // rebuild keeps the replay
            cam = service.Inputs[0];
            replay = service.Inputs[1];
            Assert.Equal("Camera 1", cam.Name);

            string dir = Path.Combine(Path.GetTempPath(), "mixit_managed_multiview");
            if (Directory.Exists(dir)) Directory.Delete(dir, true);
            Directory.CreateDirectory(dir);
            ProductionOutput mv = service.AddOutput("Multiview", new OutputSettings
            {
                Url = Path.Combine(dir, "multiview.mkv"), SourceInputId = OutputSettings.MultiviewSource, VideoKbps = 1500,
            });
            Assert.True(mv.IsMultiview);

            await service.StartAsync();
            service.StartOutput(mv.Id);
            await Task.Delay(1500);
            long mark = service.MarkReplayIn(replay.Id);
            await Task.Delay(500);
            service.PlayReplay(replay.Id, 0.5f);
            service.SetProgram(replay.Id);
            await Task.Delay(1000);
            ReplayStatus status = service.GetReplayStatus(replay.Id);
            Assert.Equal(cam.Id, status.SourceInputId);
            Assert.Equal(mark, status.MarkNs);
            Assert.True(status.State.Active);
            Assert.Equal(0.5f, status.State.Speed);
            Assert.InRange((status.State.PositionNs - mark) / 1e9, 0.3, 0.8);
            Assert.True(status.Buffer.Frames > 30);
            service.ControlReplay(replay.Id, null, true);
            Assert.True(service.GetReplayStatus(replay.Id).State.Paused);
            OutputStatistics? o = service.GetOutputStatistics(mv.Id);
            Assert.NotNull(o);
            Assert.True(o.State is OutputState.Running or OutputState.Starting, $"multiview output {o.State}: {o.LastError}");
            await service.StopAsync();
            Assert.True(new FileInfo(Path.Combine(dir, "multiview.mkv")).Length > 10_000);

            service.RemoveInput(cam.Id);  // its replay goes with it
            Assert.Empty(service.Inputs);
        }
    }

    [Fact]
    public async Task PluginInsertsAndTheirSettingsSurviveAFormatChange()
    {
        string? bundle = Vst3Tests.TestBundle();
        if (bundle is null) return;  // test plug-in not built next to this engine
        (EngineHost host, ProductionService service) = Create();
        using (host)
        using (service)
        {
            ProductionInput a = service.AddTestSignal();
            Vst3Plugin plugin = Assert.Single(await service.ScanPluginsAsync(bundle));
            await service.SetInsertAsync(a.Id, 1, plugin);
            AudioInsertParameter gain = service.GetInsertParameters(a.Id, 1)[0];
            service.SetInsertParameter(a.Id, 1, gain.Id, 0.75);
            service.SetInsertBypass(a.Id, 1, true);

            service.SetFormat(ProductionFormat.Standard.First(f => f.Width == 1280));  // rebuilds the session
            uint id = service.Inputs[0].Id;
            IReadOnlyList<AudioInsert?> inserts = service.GetInserts(id);
            Assert.Null(inserts[0]);
            Assert.Equal("MIX IT Test Gain", inserts[1]!.Name);
            Assert.True(inserts[1]!.Bypass);
            Assert.Equal("1.50", service.GetInsertParameters(id, 1)[0].Display);

            service.RemoveInsert(id, 1);
            Assert.Null(service.GetInserts(id)[1]);
        }
    }

    [Fact]
    public async Task MonitorOutputAndAudioInputUseTheTestDevice()
    {
        (EngineHost host, ProductionService service) = Create();
        using (host)
        using (service)
        {
            ProductionInput mic = service.AddAudioCapture(
                new AudioDevice(AudioDevice.NullDeviceId, "Test mic", AudioDeviceDirection.Capture, false, 2, 48000));
            Assert.Equal(ProductionInputKind.AudioCapture, mic.Kind);
            Assert.False(service.GetAudioStrip(mic.Id).AudioFollowsVideo);
            service.SetAudioOutput(AudioDevice.NullDeviceId + ":150");
            await service.StartAsync();
            await Task.Delay(1500);
            AudioOutputStatistics? o = service.GetAudioOutputStatistics();
            Assert.NotNull(o);
            Assert.Equal(AudioOutputState.Running, o.State);
            Assert.Equal(AudioBus.Monitor, o.Bus);
            Assert.Equal(0UL, o.Underruns);
            AudioMeterReading? m = service.GetInputMeter(mic.Id);
            Assert.NotNull(m);
            Assert.InRange(m.PeakDb[0], -22f, -18f);  // the test device's -20 dBFS tone
            await service.StopAsync();

            service.SetFormat(ProductionFormat.Standard.First(f => f.Width == 1280));  // output survives the rebuild
            Assert.Equal(AudioDevice.NullDeviceId + ":150", service.AudioOutputDeviceId);
            Assert.Equal(AudioOutputState.Running, service.GetAudioOutputStatistics()?.State);
            service.SetAudioOutput(null);
            Assert.Equal(AudioOutputState.Off, service.GetAudioOutputStatistics()?.State);
        }
    }
}
