# ADR-0012: One command layer; remote control on Kestrel; tally, PTZ and extensions on top

- Status: Accepted (M7)
- Date: 2026-11-21

## Context

Milestone 7 adds macros, automation, a remote-control API, tally, PTZ and a
plug-in SDK. Each of these needs to "press the operator's buttons". If each
called `ProductionService` its own way, behaviour, validation and logging
would drift, and the single-threaded `ProductionService` (owned by the UI
thread) would be called from network and timer threads.

## Decision

1. **One command layer** (`MixIt.Core.Control.CommandService`): named
   commands with described parameters, string arguments, a `CommandResult`
   that never throws for operator errors, and an `Executed` event. Macros,
   automation, the remote API and extensions all go through it. Every handler
   runs on the production thread via `IProductionDispatcher` (WPF dispatcher
   in the app; a re-entrant serial dispatcher in tests/headless use).
2. **Macros** are plain text (`Command key=value`), stored readable in
   `%AppData%\MIX IT\control.json`; nesting (8) and concurrency (32) limits;
   cancellable waits that do not hold the production thread.
3. **Automation only when armed.** Rules (daily, once, interval, production
   start/stop) run macros; nothing fires while disarmed and missed times are
   not caught up (CLAUDE.md §8: no unattended changes without approval).
4. **Remote control on Kestrel** (ASP.NET Core shared framework, MIT) in its
   own project `MixIt.Remote`, rather than `HttpListener`: binding to the LAN
   with `HttpListener` on Windows needs an administrator URL reservation; Kestrel
   does not, and its WebSocket and limits support is mature. Cost: the
   self-contained package carries the ASP.NET Core runtime (~10 MB).
   Security: random 256-bit bearer token compared in constant time (hashes),
   localhost by default, 64 KiB bodies, 64 connections, 32 event clients each
   with a bounded drop-oldest queue so no client can stall the production.
   Plain HTTP: documented as trusted-LAN only (TLS later if needed).
5. **Tally** is computed from engine state (program bus, a running
   transition's incoming input, PiP, visible overlays) and sent as **TSL UMD
   v5.0** over UDP (publicly documented, widely supported), resent every
   second; send failures are logged and never propagate.
6. **PTZ** speaks **VISCA over IP** (Sony framing on UDP 52381, raw VISCA on
   UDP/TCP for PTZOptics-style cameras) fire-and-forget with a 2 s TCP timeout.
7. **Extensions** (`MixIt.SDK`, depends only on `MixIt.Protocol`) load from
   `%ProgramData%\MIX IT\Extensions\<Name>\<Name>.dll`, each in its own
   `AssemblyLoadContext` sharing the contract assemblies; disabled by default;
   initialization, command and event failures are caught and reported. They run
   in-process: a hostile or crashing extension can still harm the app — stated
   in the UI.

## Consequences

- New control surfaces (Stream Deck, MIDI, OSC, companion apps) only need the
  remote API or an extension; no new paths into the production.
- Tally and PTZ are implemented against public protocols but no hardware was
  available: EXPERIMENTAL until tested with real devices.
- Out-of-process extensions, OSC/MIDI, HTTPS and user accounts are future work.
