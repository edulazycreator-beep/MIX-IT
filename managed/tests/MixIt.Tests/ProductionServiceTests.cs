using System.Runtime.InteropServices;
using Microsoft.Extensions.Logging.Abstractions;
using MixIt.Core.Engine;
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
            Assert.Equal(1, service.Inputs.Count);
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
}
