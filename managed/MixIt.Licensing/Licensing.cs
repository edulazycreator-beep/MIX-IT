using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace MixIt.Licensing;

public enum Edition
{
    Free,
    Professional,
}

/// <summary>What a licence grants (the signed payload).</summary>
public sealed record LicenseInfo
{
    public required string LicenseId { get; init; }
    public required string Licensee { get; init; }
    public required Edition Edition { get; init; }
    public required DateOnly Issued { get; init; }
    /// <summary>Last valid day (null = perpetual).</summary>
    public DateOnly? Expires { get; init; }
    /// <summary>Bound to one PC (<see cref="MachineIdentity.Current"/>), or null for any PC.</summary>
    public string? MachineId { get; init; }
    public int Version { get; init; } = 1;
}

/// <summary>Result of checking a licence file.</summary>
public sealed record LicenseCheck(bool Valid, LicenseInfo? License, string? Problem)
{
    public static LicenseCheck Invalid(string problem, LicenseInfo? license = null) => new(false, license, problem);
}

/// <summary>
/// What each edition may do. Checked when the production is configured (adding
/// inputs/outputs, enabling features) — never during a running show.
/// The defaults are a proposal for the owner to confirm (ADR-0013).
/// </summary>
public sealed record EditionPolicy
{
    public required Edition Edition { get; init; }
    /// <summary>Video inputs (cameras, files, graphics, replays); audio-only inputs are not counted.</summary>
    public int MaxVideoInputs { get; init; } = int.MaxValue;
    /// <summary>Simultaneous stream destinations (files are not limited).</summary>
    public int MaxStreams { get; init; } = int.MaxValue;
    public bool IsoRecording { get; init; } = true;
    public bool InstantReplay { get; init; } = true;
    public bool MultiviewOutput { get; init; } = true;
    public bool RemoteControl { get; init; } = true;
    public bool Extensions { get; init; } = true;

    public static EditionPolicy Professional { get; } = new() { Edition = Edition.Professional };

    public static EditionPolicy Free { get; } = new()
    {
        Edition = Edition.Free,
        MaxVideoInputs = 4,
        MaxStreams = 1,
        IsoRecording = false,
        InstantReplay = false,
        MultiviewOutput = false,
        RemoteControl = false,
        Extensions = false,
    };

    /// <summary>Message for a refused action.</summary>
    public string Refusal(string what) => $"{what} needs MIX IT Professional (this is the {Edition} edition).";
}

/// <summary>Licence files: a JSON payload and an ECDSA P-256 / SHA-256 signature, both base64.</summary>
public static class LicenseFormat
{
    internal static readonly JsonSerializerOptions Json = new(JsonSerializerDefaults.Web)
    {
        Converters = { new JsonStringEnumConverter() },
    };

    private sealed record Envelope(string Product, string Payload, string Signature);

    /// <summary>Creates a key pair: (private key PKCS#8 PEM — keep secret, public key base64 SPKI — goes into the build).</summary>
    public static (string PrivateKeyPem, string PublicKeyBase64) CreateKeyPair()
    {
        using var key = ECDsa.Create(ECCurve.NamedCurves.nistP256);
        return (key.ExportPkcs8PrivateKeyPem(), Convert.ToBase64String(key.ExportSubjectPublicKeyInfo()));
    }

    /// <summary>Signs a licence (publisher side only).</summary>
    public static string Issue(LicenseInfo license, string privateKeyPem)
    {
        ArgumentNullException.ThrowIfNull(license);
        using var key = ECDsa.Create();
        key.ImportFromPem(privateKeyPem);
        byte[] payload = JsonSerializer.SerializeToUtf8Bytes(license, Json);
        byte[] signature = key.SignData(payload, HashAlgorithmName.SHA256, DSASignatureFormat.Rfc3279DerSequence);
        return JsonSerializer.Serialize(new Envelope("MIX IT", Convert.ToBase64String(payload), Convert.ToBase64String(signature)), Json);
    }

    /// <summary>Verifies a licence file against the publisher key, today's date and this PC.</summary>
    public static LicenseCheck Verify(string text, string? publicKeyBase64, DateOnly today, string machineId)
    {
        ArgumentNullException.ThrowIfNull(text);
        if (string.IsNullOrWhiteSpace(publicKeyBase64))
            return LicenseCheck.Invalid("This build has no licence key configured, so licences cannot be verified (Free edition).");
        if (text.Length > 16 * 1024) return LicenseCheck.Invalid("Not a MIX IT licence (file too large).");
        Envelope? envelope;
        byte[] payload, signature, publicKey;
        try
        {
            envelope = JsonSerializer.Deserialize<Envelope>(text, Json);
            if (envelope is null || envelope.Product != "MIX IT") return LicenseCheck.Invalid("Not a MIX IT licence.");
            payload = Convert.FromBase64String(envelope.Payload);
            signature = Convert.FromBase64String(envelope.Signature);
            publicKey = Convert.FromBase64String(publicKeyBase64);
        }
        catch (Exception ex) when (ex is JsonException or FormatException or ArgumentNullException)
        {
            return LicenseCheck.Invalid("Not a MIX IT licence (damaged file).");
        }
        using (var key = ECDsa.Create())
        {
            try
            {
                key.ImportSubjectPublicKeyInfo(publicKey, out _);
            }
            catch (CryptographicException)
            {
                return LicenseCheck.Invalid("The licence key of this build is invalid.");
            }
            if (!key.VerifyData(payload, signature, HashAlgorithmName.SHA256, DSASignatureFormat.Rfc3279DerSequence))
                return LicenseCheck.Invalid("The licence signature is not valid (changed or not issued for MIX IT).");
        }
        LicenseInfo? license;
        try
        {
            license = JsonSerializer.Deserialize<LicenseInfo>(payload, Json);
        }
        catch (JsonException)
        {
            return LicenseCheck.Invalid("The licence content cannot be read.");
        }
        if (license is null) return LicenseCheck.Invalid("The licence content cannot be read.");
        if (license.Version > 1) return LicenseCheck.Invalid("This licence needs a newer MIX IT.", license);
        if (license.Expires is DateOnly end && today > end) return LicenseCheck.Invalid($"The licence expired on {end:yyyy-MM-dd}.", license);
        if (license.Issued > today.AddDays(2)) return LicenseCheck.Invalid("The licence is dated in the future (check the PC clock).", license);
        if (license.MachineId is not null && !string.Equals(license.MachineId, machineId, StringComparison.OrdinalIgnoreCase))
            return LicenseCheck.Invalid("The licence belongs to another PC.", license);
        return new LicenseCheck(true, license, null);
    }

    /// <summary>The publisher key built into this assembly (empty when none was configured).</summary>
    public static string? BuiltInPublicKey() => typeof(LicenseFormat).Assembly.GetCustomAttributes<AssemblyMetadataAttribute>()
        .FirstOrDefault(a => a.Key == "MixItLicensePublicKey")?.Value is { Length: > 0 } k ? k : null;
}

/// <summary>A stable, anonymous id of this PC (hash; no hardware details leave the PC).</summary>
public static class MachineIdentity
{
    public static string Current()
    {
        string source = Environment.MachineName;
        if (OperatingSystem.IsWindows())
        {
            try
            {
                using var key = Microsoft.Win32.Registry.LocalMachine.OpenSubKey(@"SOFTWARE\Microsoft\Cryptography");
                if (key?.GetValue("MachineGuid") is string guid) source = guid;
            }
            catch (Exception ex) when (ex is System.Security.SecurityException or UnauthorizedAccessException or IOException)
            {
                // fall back to the machine name
            }
        }
        byte[] hash = SHA256.HashData(Encoding.UTF8.GetBytes("MIX IT machine v1|" + source));
        return Convert.ToHexString(hash, 0, 10);
    }
}

/// <summary>
/// The installed licence and the resulting edition (M9). The licence file lives
/// in <c>%ProgramData%\MIX IT\license.mixit-license</c> (all users of the PC).
/// </summary>
public sealed class LicenseService
{
    private readonly string _path;
    private readonly string? _publicKey;
    private readonly Func<DateOnly> _today;

    public LicenseService(string path, string? publicKey, Func<DateOnly>? today = null)
    {
        _path = path;
        _publicKey = publicKey;
        _today = today ?? (() => DateOnly.FromDateTime(DateTime.Now));
        Status = Check(File.Exists(path) ? SafeRead(path) : null);
    }

    public static string DefaultPath() =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "MIX IT", "license.mixit-license");

    public event EventHandler? Changed;

    /// <summary>Last check of the installed licence (Valid = false and Problem = null when none is installed).</summary>
    public LicenseCheck Status { get; private set; }

    public Edition Edition => Status is { Valid: true, License: { } l } ? l.Edition : Edition.Free;

    public EditionPolicy Policy => Edition == Edition.Professional ? EditionPolicy.Professional : EditionPolicy.Free;

    public bool CanVerify => _publicKey is not null;

    public string MachineId { get; } = MachineIdentity.Current();

    /// <summary>Checks and installs a licence; the previous one stays when the new one is invalid.</summary>
    public LicenseCheck Install(string text)
    {
        LicenseCheck check = LicenseFormat.Verify(text, _publicKey, _today(), MachineId);
        if (!check.Valid) return check;
        Directory.CreateDirectory(Path.GetDirectoryName(_path)!);
        File.WriteAllText(_path + ".tmp", text);
        File.Move(_path + ".tmp", _path, overwrite: true);
        Status = check;
        Changed?.Invoke(this, EventArgs.Empty);
        return check;
    }

    public void Remove()
    {
        if (File.Exists(_path)) File.Delete(_path);
        Status = new LicenseCheck(false, null, null);
        Changed?.Invoke(this, EventArgs.Empty);
    }

    /// <summary>Re-checks (e.g. daily) — an expired licence becomes Free for the next configuration, never mid-show.</summary>
    public void Recheck()
    {
        LicenseCheck before = Status;
        Status = Check(File.Exists(_path) ? SafeRead(_path) : null);
        if (Status.Valid != before.Valid || Status.License?.Edition != before.License?.Edition) Changed?.Invoke(this, EventArgs.Empty);
    }

    private LicenseCheck Check(string? text) =>
        text is null ? new LicenseCheck(false, null, null) : LicenseFormat.Verify(text, _publicKey, _today(), MachineId);

    private static string? SafeRead(string path)
    {
        try
        {
            return File.ReadAllText(path);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            return null;
        }
    }
}
