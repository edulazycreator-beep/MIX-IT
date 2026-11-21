using System.Globalization;
using MixIt.Licensing;

// mixit-license keygen <folder>
// mixit-license issue --key <private.pem> --to "<licensee>" [--edition Professional] [--expires yyyy-MM-dd] [--machine <id>] --out <file>
// mixit-license verify --public <base64> <file> [--machine <id>]
if (args.Length == 0) return Usage();
try
{
    switch (args[0])
    {
        case "keygen":
        {
            if (args.Length != 2) return Usage();
            Directory.CreateDirectory(args[1]);
            string priv = Path.Combine(args[1], "mixit-license-private.pem");
            if (File.Exists(priv)) throw new InvalidOperationException($"{priv} exists: refusing to overwrite a signing key.");
            (string pem, string pub) = LicenseFormat.CreateKeyPair();
            File.WriteAllText(priv, pem);
            File.WriteAllText(Path.Combine(args[1], "mixit-license-public.txt"), pub);
            Console.WriteLine($"Private key: {priv}  (keep it secret and backed up; never commit it)");
            Console.WriteLine("Public key (set as repository variable MIXIT_LICENSE_PUBLIC_KEY):");
            Console.WriteLine(pub);
            return 0;
        }
        case "issue":
        {
            string key = Option("--key") ?? throw new ArgumentException("--key is required");
            string to = Option("--to") ?? throw new ArgumentException("--to is required");
            string output = Option("--out") ?? throw new ArgumentException("--out is required");
            var license = new LicenseInfo
            {
                LicenseId = Guid.NewGuid().ToString("N"),
                Licensee = to,
                Edition = Enum.Parse<Edition>(Option("--edition") ?? "Professional", ignoreCase: true),
                Issued = DateOnly.FromDateTime(DateTime.UtcNow),
                Expires = Option("--expires") is { } e ? DateOnly.ParseExact(e, "yyyy-MM-dd", CultureInfo.InvariantCulture) : null,
                MachineId = Option("--machine"),
            };
            File.WriteAllText(output, LicenseFormat.Issue(license, File.ReadAllText(key)));
            Console.WriteLine($"Issued {license.Edition} licence {license.LicenseId} for {license.Licensee} → {output}");
            return 0;
        }
        case "verify":
        {
            string pub = Option("--public") ?? throw new ArgumentException("--public is required");
            string file = args[^1];
            LicenseCheck check = LicenseFormat.Verify(File.ReadAllText(file), pub, DateOnly.FromDateTime(DateTime.Now),
                Option("--machine") ?? MachineIdentity.Current());
            Console.WriteLine(check.Valid ? $"VALID: {check.License}" : $"INVALID: {check.Problem}");
            return check.Valid ? 0 : 1;
        }
        case "machine-id":
            Console.WriteLine(MachineIdentity.Current());
            return 0;
        default:
            return Usage();
    }
}
catch (Exception ex) when (ex is ArgumentException or InvalidOperationException or IOException or FormatException or System.Security.Cryptography.CryptographicException)
{
    Console.Error.WriteLine($"error: {ex.Message}");
    return 2;
}

string? Option(string name)
{
    int i = Array.IndexOf(args, name);
    return i >= 0 && i + 1 < args.Length ? args[i + 1] : null;
}

static int Usage()
{
    Console.Error.WriteLine("""
        mixit-license keygen <folder>
        mixit-license issue --key <private.pem> --to "<licensee>" [--edition Professional|Free] [--expires yyyy-MM-dd] [--machine <id>] --out <file>
        mixit-license verify --public <base64 key> [--machine <id>] <file>
        mixit-license machine-id
        """);
    return 1;
}
