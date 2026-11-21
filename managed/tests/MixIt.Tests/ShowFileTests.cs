using System.Text;
using Microsoft.Extensions.Logging.Abstractions;
using MixIt.Core.Engine;
using MixIt.Core.Graphics;
using MixIt.Core.Production;
using MixIt.Interop;
using Xunit;

namespace MixIt.Tests;

/// <summary>M8: show files (save / open a production).</summary>
public class ShowFileTests
{
    private sealed class ReversingProtector : ISecretProtector
    {
        public string Protect(string secret) => Convert.ToBase64String(Encoding.UTF8.GetBytes(new string(secret.Reverse().ToArray())));
        public string Unprotect(string p) => new(Encoding.UTF8.GetString(Convert.FromBase64String(p)).Reverse().ToArray());
    }

    private static (EngineHost, ProductionService) Create()
    {
        var host = new EngineHost(new EngineHostOptions(), NullLogger<EngineHost>.Instance);
        host.Start();
        return (host, new ProductionService(host, NullLogger<ProductionService>.Instance));
    }

    [Fact]
    public void AProductionSurvivesSaveAndOpen()
    {
        string dir = Path.Combine(Path.GetTempPath(), "mixit_show_test");
        if (Directory.Exists(dir)) Directory.Delete(dir, true);
        Directory.CreateDirectory(dir);
        string file = Path.Combine(dir, "wedding" + ShowFile.Extension);
        const string Rtmp = "rtmp://live.example.com/app/SECRET-KEY-123";

        (EngineHost host, ProductionService p) = Create();
        using (host)
        using (p)
        {
            p.SetFormat(ProductionFormat.Standard.First(f => f.Width == 1280));
            ProductionInput cam1 = p.AddTestSignal();
            ProductionInput cam2 = p.AddTestSignal();
            p.RenameInput(cam1.Id, "Altar");
            GraphicsTemplate lt = GraphicsTemplates.All.First(t => t.Id == "lower-third");
            ProductionInput title = p.AddGraphics("Names", lt.SceneJson, lt.Fields);
            p.SetGraphicsField(title.Id, "name", "Rahma & Dimas");
            p.AddReplay(cam1.Id, 30);
            p.SetOverlay(1, title.Id, KeySettings.None, new NormalizedRect(0, 0.5f, 1, 0.5f), 0.9f);
            p.SetPictureInPicture(cam2.Id);
            p.SetAudioStrip(cam2.Id, p.GetAudioStrip(cam2.Id) with { FaderDb = -12f, Mute = true });
            p.AddOutput("Recording", new OutputSettings { Url = Path.Combine(dir, "rec.mp4"), VideoKbps = 8000 });
            p.AddOutput("YouTube", new OutputSettings { Url = Rtmp });
            p.AddOutput("ISO 2", new OutputSettings { Url = Path.Combine(dir, "iso2.mkv") }, cam2.Id);
            ShowFile.Save(p.ExportShow(new ReversingProtector()), file);
            ShowFile.Save(p.ExportShow(null), Path.Combine(dir, "nokeys.mixit"));
        }
        string text = File.ReadAllText(file);
        Assert.DoesNotContain("SECRET-KEY-123", text);  // the key is only stored protected
        Assert.Contains("Rahma & Dimas", text);

        (EngineHost host2, ProductionService q) = Create();
        using (host2)
        using (q)
        {
            q.AddTestSignal();  // replaced by the show
            IReadOnlyList<string> warnings = q.ImportShow(ShowFile.Load(file), new ReversingProtector());
            Assert.Empty(warnings);
            Assert.Equal(1280u, q.Format.Width);
            Assert.Equal("Altar|Test signal 2|Names|Replay Altar", string.Join("|", q.Inputs.Select(i => i.Name)));
            Assert.Equal(ProductionInputKind.Replay, q.Inputs[3].Kind);
            Assert.Equal("Rahma & Dimas", q.GetGraphicsFields(q.Inputs[2].Id)["name"]);
            (uint? onChannel, _, _) = q.GetOverlay(1);
            Assert.Equal(q.Inputs[2].Id, onChannel);
            Assert.Equal(new NormalizedRect(0, 0.5f, 1, 0.5f), q.GetOverlayDestination(1));
            Assert.Equal(q.Inputs[1].Id, q.PictureInPictureInputId);
            Assert.Equal(-12f, q.GetAudioStrip(q.Inputs[1].Id).FaderDb);
            Assert.True(q.GetAudioStrip(q.Inputs[1].Id).Mute);
            Assert.Equal(3, q.Outputs.Count);
            Assert.Equal(Rtmp, q.Outputs[1].Settings.Url);
            Assert.Equal(8000, q.Outputs[0].Settings.VideoKbps);
            Assert.Equal(q.Inputs[1].Id, q.Outputs[2].SourceInputId);

            // Without a key store the key is not saved, and opening says so.
            IReadOnlyList<string> w2 = q.ImportShow(ShowFile.Load(Path.Combine(dir, "nokeys.mixit")), null);
            Assert.Contains(w2, w => w.Contains("YouTube", StringComparison.Ordinal) && w.Contains("enter it again", StringComparison.Ordinal));
            Assert.DoesNotContain("SECRET-KEY-123", File.ReadAllText(Path.Combine(dir, "nokeys.mixit")));

            // A missing media file is reported, the rest opens.
            var show = new ShowDocument
            {
                Format = new ShowFormat("x", 640, 360, 30, 1),
                Inputs = [new ShowInput(ProductionInputKind.MediaFile, "Gone", Path.Combine(dir, "missing.mp4"), true), new ShowInput(ProductionInputKind.TestSignal, "T", null, false)],
                Overlays = [new ShowOverlay(0, KeySettings.None, NormalizedRect.Full, 1)],
            };
            IReadOnlyList<string> w3 = q.ImportShow(show, null);
            Assert.Single(q.Inputs);
            Assert.Contains(w3, w => w.StartsWith("Gone: file not found", StringComparison.Ordinal));
            Assert.Contains(w3, w => w.StartsWith("Overlay 1", StringComparison.Ordinal));
            File.WriteAllText(Path.Combine(dir, "bad.mixit"), "{\"product\":\"other\",\"format\":{\"name\":\"x\",\"width\":1,\"height\":1,\"rateNumerator\":1,\"rateDenominator\":1}}");
            Assert.Throws<InvalidDataException>(() => ShowFile.Load(Path.Combine(dir, "bad.mixit")));
        }
    }
}
