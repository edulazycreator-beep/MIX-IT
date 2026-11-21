using System.Text;
using System.Text.Json;
using Microsoft.Extensions.Logging.Abstractions;
using MixIt.Core.Engine;
using MixIt.Core.Production;
using MixIt.Interop;
using MixIt.Licensing;
using Xunit;

namespace MixIt.Tests;

/// <summary>M9: signed licence files, editions and their limits.</summary>
public class LicensingTests
{
    private static readonly DateOnly Today = new(2026, 11, 21);

    private static LicenseInfo Pro(string? machine = null, DateOnly? expires = null) => new()
    {
        LicenseId = "test-1", Licensee = "Studio Contoh", Edition = Edition.Professional,
        Issued = Today, Expires = expires, MachineId = machine,
    };

    [Fact]
    public void SignedLicencesVerifyAndTamperingIsDetected()
    {
        (string priv, string pub) = LicenseFormat.CreateKeyPair();
        (_, string otherPub) = LicenseFormat.CreateKeyPair();
        string file = LicenseFormat.Issue(Pro(), priv);
        Assert.DoesNotContain("PRIVATE", file);

        LicenseCheck ok = LicenseFormat.Verify(file, pub, Today, "PC1");
        Assert.True(ok.Valid);
        Assert.Equal("Studio Contoh", ok.License!.Licensee);

        Assert.Contains("signature is not valid", LicenseFormat.Verify(file, otherPub, Today, "PC1").Problem);
        Assert.Contains("no licence key", LicenseFormat.Verify(file, null, Today, "PC1").Problem);
        Assert.Contains("Not a MIX IT licence", LicenseFormat.Verify("hello", pub, Today, "PC1").Problem);

        // Change the payload (Free → Professional style edits) without re-signing.
        using JsonDocument doc = JsonDocument.Parse(file);
        string payload = Encoding.UTF8.GetString(Convert.FromBase64String(doc.RootElement.GetProperty("payload").GetString()!));
        string forged = file.Replace(doc.RootElement.GetProperty("payload").GetString()!,
            Convert.ToBase64String(Encoding.UTF8.GetBytes(payload.Replace("Studio Contoh", "Someone Else", StringComparison.Ordinal))), StringComparison.Ordinal);
        Assert.False(LicenseFormat.Verify(forged, pub, Today, "PC1").Valid);

        Assert.Contains("expired on 2026-11-20", LicenseFormat.Verify(LicenseFormat.Issue(Pro(expires: Today.AddDays(-1)), priv), pub, Today, "PC1").Problem);
        Assert.True(LicenseFormat.Verify(LicenseFormat.Issue(Pro(expires: Today), priv), pub, Today, "PC1").Valid);
        Assert.Contains("another PC", LicenseFormat.Verify(LicenseFormat.Issue(Pro(machine: "PC2"), priv), pub, Today, "PC1").Problem);
        Assert.True(LicenseFormat.Verify(LicenseFormat.Issue(Pro(machine: "pc1"), priv), pub, Today, "PC1").Valid);
        Assert.Equal(20, MachineIdentity.Current().Length);
    }

    [Fact]
    public void TheLicenceServiceInstallsKeepsAndRemoves()
    {
        (string priv, string pub) = LicenseFormat.CreateKeyPair();
        string dir = Path.Combine(Path.GetTempPath(), "mixit_license_test");
        if (Directory.Exists(dir)) Directory.Delete(dir, true);
        string path = Path.Combine(dir, "license.mixit-license");
        var service = new LicenseService(path, pub, () => Today);
        Assert.Equal(Edition.Free, service.Edition);
        Assert.Null(service.Status.Problem);
        Assert.Equal(4, service.Policy.MaxVideoInputs);

        Assert.False(service.Install("garbage").Valid);
        Assert.False(File.Exists(path));
        int changes = 0;
        service.Changed += (_, _) => changes++;
        Assert.True(service.Install(LicenseFormat.Issue(Pro(), priv)).Valid);
        Assert.Equal(Edition.Professional, service.Edition);
        Assert.Equal(1, changes);
        Assert.Equal(Edition.Professional, new LicenseService(path, pub, () => Today).Edition);  // persisted
        Assert.Equal(Edition.Free, new LicenseService(path, null, () => Today).Edition);  // a build without the key cannot verify

        // An invalid new licence keeps the current one.
        Assert.False(service.Install(LicenseFormat.Issue(Pro(expires: Today.AddDays(-3)), priv)).Valid);
        Assert.Equal(Edition.Professional, service.Edition);

        service.Remove();
        Assert.Equal(Edition.Free, service.Edition);
        Assert.False(File.Exists(path));
    }

    [Fact]
    public void TheFreeEditionLimitsConfigurationNotARunningShow()
    {
        using var host = new EngineHost(new EngineHostOptions(), NullLogger<EngineHost>.Instance);
        host.Start();
        using var p = new ProductionService(host, NullLogger<ProductionService>.Instance) { Policy = EditionPolicy.Free };
        for (int i = 0; i < 4; i++) p.AddTestSignal();
        InvalidOperationException ex = Assert.Throws<InvalidOperationException>(() => p.AddTestSignal());
        Assert.Contains("needs MIX IT Professional", ex.Message);
        Assert.Throws<InvalidOperationException>(() => p.AddReplay(p.Inputs[0].Id));
        Assert.Throws<InvalidOperationException>(() => p.AddOutput("ISO", new OutputSettings { Url = Path.Combine(Path.GetTempPath(), "iso.mkv") }, p.Inputs[0].Id));
        Assert.Throws<InvalidOperationException>(() => p.AddOutput("MV", new OutputSettings { Url = Path.Combine(Path.GetTempPath(), "mv.mkv"), SourceInputId = OutputSettings.MultiviewSource }));
        p.AddOutput("Stream 1", new OutputSettings { Url = "rtmp://127.0.0.1/live/key" });
        Assert.Throws<InvalidOperationException>(() => p.AddOutput("Stream 2", new OutputSettings { Url = "srt://127.0.0.1:9000" }));
        p.AddOutput("Recording", new OutputSettings { Url = Path.Combine(Path.GetTempPath(), "rec.mp4") });  // files are not limited
        Assert.Equal(2, p.Outputs.Count);

        p.Policy = EditionPolicy.Professional;
        p.AddTestSignal();
        p.AddReplay(p.Inputs[0].Id);
        Assert.Equal(6, p.Inputs.Count);
        p.Policy = EditionPolicy.Free;  // dropping back changes nothing that exists
        Assert.Equal(6, p.Inputs.Count);
    }
}
