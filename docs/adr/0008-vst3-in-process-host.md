# ADR-0008: VST3 effects hosted in the engine process (first version)

- Status: Accepted (M4, EXPERIMENTAL)
- Date: 2026-11-19

## Context

M4 calls for VST3 plug-in hosting on mixer strips. Steinberg published the VST 3
SDK under the MIT licence from 3.8.0, so the interface headers can be used in a
closed-source product. The SDK's hosting helpers (`public.sdk/source/vst/hosting`)
are also MIT, but they pull in a large amount of code we would need to review,
pin and maintain.

A plug-in is third-party native code. Loaded into the engine process, a crash or
memory corruption in a plug-in takes the whole production down — the main risk
of plug-in hosting in a live product.

## Decision

1. **Interfaces only.** We pin `vst3_pluginterfaces` (tag `v3.8.1_build_84`,
   commit `4f547e8e…`) and write our own host (`native/src/audio/vst3_host.*`):
   module loading (`LoadLibraryExW` + `InitDll`/`ExitDll` on Windows,
   `dlopen` + `ModuleEntry`/`ModuleExit` on Linux), factory scanning
   ("Audio Module Class"), host objects (IHostApplication, IBStream,
   IParameterChanges, IComponentHandler) and the processing lifecycle.
   `MIXIT_ENABLE_VST3=OFF` builds the engine without it (calls return
   NOT_SUPPORTED).
2. **In-process for now.** Effects run on the engine's audio thread, in the
   engine process. The host catches C++ exceptions around every plug-in call
   and counts failed blocks (the input passes through), but it cannot survive
   access violations or memory corruption.
3. **Two insert slots per input strip, before the strip DSP** (trim, HPF, EQ,
   dynamics). Mono and stereo sessions; 32-bit float; real-time mode; fixed
   10 ms blocks.
4. **Hand-over without locks on the audio thread.** Each slot is an atomic
   pointer. Loading happens on the caller's thread; the new effect is swapped in
   and a replaced/removed effect is destroyed only after the audio thread has
   finished two more blocks (epoch counter). If the audio thread does not
   advance within 2 s (stuck inside a plug-in), the effect is leaked rather
   than destroyed under it.
5. **Parameters** go through a latest-value table (one slot per parameter):
   it never overflows and only the newest value per block is delivered, at
   sample offset 0. With no audio thread running the session delivers them
   with a zero-length process call, so a saved state contains them.
6. **State** = component state + controller state in one blob ("MXV3"),
   which the application keeps and re-applies when it rebuilds a session.
7. A real VST3 test plug-in (`native/tests/vst3_test_plugin`, written on the
   interface headers) is built with the tests on every platform, so the host is
   tested end to end in CI.

## Consequences

- Operators get plug-in effects now; the UI labels them EXPERIMENTAL and warns
  that a faulty plug-in can stop the engine.
- Not yet: plug-in editors (IPlugView windows), sidechains, MIDI/events,
  latency compensation (reported latency is shown, not compensated), host
  message objects (`IHostApplication::createInstance` returns kNotImplemented,
  which some plug-ins need for controller ↔ processor messages).
- Only the test plug-in has been exercised. Commercial plug-ins on Windows are
  untested; compatibility testing is part of M9.

## Follow-up

- **Out-of-process plug-in host** (a separate `mixit-plughost` process per
  plug-in or group, shared-memory audio ring, watchdog and automatic bypass on
  crash) before a commercial release. It changes only the inside of
  `Vst3Effect`; the session and ABI stay the same.
- Plug-in editor windows (M8 desktop experience).
