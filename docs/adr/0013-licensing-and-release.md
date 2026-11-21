# ADR-0013: Offline signed licences, edition policy, installer

- Status: Accepted (M9) — edition limits are a **proposal for the owner**
- Date: 2026-11-21

## Context

CLAUDE.md asks for a Free and a Professional edition, modular licensing, no
hard-coded secrets, and that production is never disrupted. The owner has not
yet decided on an activation server, prices or the exact Free limits, nor
provided a code-signing certificate.

## Decision

1. **Offline licence files**: a JSON payload (licence id, licensee, edition,
   issue date, optional expiry, optional PC binding) signed with **ECDSA P-256
   / SHA-256** (.NET built-in cryptography, no dependency). The app holds only
   the publisher's **public** key, injected at build time from the repository
   variable `MIXIT_LICENSE_PUBLIC_KEY`. The private key is created and kept by
   the owner with `mixit-license keygen` (tool not shipped); it never enters the
   repository or CI. A build without the variable cannot verify licences and
   runs as Free — stated in the License section.
2. **PC binding** uses an anonymous hash of the Windows MachineGuid; nothing is
   sent anywhere. An online activation service can later issue the same files
   (adapter point: `LicenseService.Install`).
3. **Edition policy** (`EditionPolicy`) is one table, enforced where the
   production is configured (adding inputs/outputs, enabling replay, remote,
   extensions). **Never while a show runs**: an expired licence changes nothing
   until the next configuration. Proposed Free tier: 4 video inputs, 1 stream,
   unlimited recordings, no ISO/replay/multiview output/remote/extensions; no
   watermark. The owner may change the table without touching enforcement.
4. **Installer**: Inno Setup 6 (free for commercial use; its setup stub is
   redistributed inside our installer) — per-machine install, Start menu,
   `.mixit` file association, licence folder writable by users, silent
   install/uninstall tested in CI. **Unsigned** until the owner provides a
   certificate (SmartScreen warns).
5. **Product version** `0.9.0` lives in `/VERSION` and `Directory.Build.props`;
   the engine C ABI keeps its own version (0.6.x).

## Consequences

- Licences cannot be forged without the private key; a copied file only works
  where its binding (if any) matches.
- Losing the private key means re-keying: a new public key in a new build and
  re-issued licences. The owner must back it up.
- Offline licences cannot be revoked; time-limited licences mitigate this until
  an activation service exists.
