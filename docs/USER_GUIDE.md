# MIX IT — User Guide (0.9 preview)

MIX IT is a live production studio for Windows 10/11 (x64): switch cameras and
media, mix audio, add titles, record and stream. This guide covers the
everyday workflow. Status of each feature (tested / experimental) is listed in
`README.txt` and in each section of the app.

> **Before a real event:** capture cards, GPU compositing, hardware encoders,
> real streaming platforms, audio devices, tally, PTZ and extra displays have
> not yet been verified on real hardware. Rehearse with your exact setup first.

## 1. First start

1. Install with `MixIt-Setup-<version>-x64.exe` (or unzip the package and run
   `MixIt.exe`). Windows may show a SmartScreen warning: the build is not
   code-signed yet.
2. **Hardware Diagnostics** shows the engine, CPU, GPU, memory and the log.
3. **License** shows the edition. Without a licence MIX IT is the Free edition.

## 2. Build a show

- **Production Studio** → choose the format (720p/1080p, 25–60 fps), add inputs:
  test signal, media file, capture device (webcam / HDMI card), audio input.
- **Input Manager** lists inputs by **number** — the number used by keyboard
  shortcuts, macros and the remote API. Rename inputs here (the name also
  appears on the multiview).
- **Graphics Studio** → add a template (lower third, title, clock, countdown,
  scoreboard, logo), edit its text live, and put it on one of 4 **overlay
  channels**. Overlays stay on air across cuts. A green/blue-screen camera on an
  overlay channel with a key and a "Presenter" placement gives a simple virtual set.
- **Streaming & Recording** → add recordings (MP4/MOV/MKV, optional ISO of one
  input, multiview) and streams (RTMP/RTMPS, SRT, HLS).
- **Settings & Workspace → Save as…** stores everything in a `.mixit` show file.
  Stream keys are encrypted for your Windows user; on another PC you re-enter them.

## 3. Go live

- **Start production**, choose the next shot on **preview** (click a tile or
  press `1`…`9`), then **CUT** (`Space`) or **AUTO** (`Enter`).
  `Ctrl+1`…`9` cuts an input straight to program; `F1`…`F4` toggle overlay channels.
- **Audio Mixer**: faders, mute/solo, EQ, dynamics, buses, loudness meter.
  Audio follows video by default for camera/media inputs.
- The **dashboard** at the bottom shows ON AIR, REC/LIVE, timing problems,
  CPU and memory. A red warning means an output has a problem — check
  Streaming & Recording.
- **Replay & Multiview**: watch every input with tally; add a replay of an input,
  press **MARK IN** at the start of the action, then **PLAY MARK** at 50 % and
  take the replay input to air.

## 4. Automate

**Automation & Control**:

- **Macros**: one command per line, e.g.
  ```
  SetText input="Names" field=name value="Rahma & Dimas"
  OverlayIn channel=1 fade=15
  Wait 6000
  OverlayOut channel=1
  ```
  The command reference is on the right. Shortcuts use the same syntax
  (Settings & Workspace → Keyboard shortcuts).
- **Automation**: rules that run macros daily, once, at intervals or when the
  production starts/stops. Nothing runs until you tick **ARMED**.
- **Remote control**: enable the API for tablets, control surfaces or scripts
  (see `REMOTE_API.md`); open `http://<pc>:8099/` for the browser panel.
  Keep it on a trusted network.
- **Tally** (TSL UMD 5.0) and **PTZ cameras** (VISCA over IP): add the device
  addresses; both are experimental.

## 5. Extra displays

Settings & Workspace → **Full-screen output**: pick a display and Program,
Preview or Multiview (for an LED wall, projector or confidence monitor).

## 6. Files and support

- Settings: `%APPDATA%\MIX IT\` · Logs: `%LOCALAPPDATA%\MixIt\logs\`
- Licence: `%ProgramData%\MIX IT\license.mixit-license`
- When reporting a problem, attach the newest log file and the steps to reproduce.
