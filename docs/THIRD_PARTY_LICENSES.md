# Third-Party Dependency Inventory

Every third-party component must be listed here **before** it is added to a
build. "Redistributed" means it ships with the product. Not legal advice;
⚖️ marks items requiring review by qualified counsel before commercial release.

## In use (Milestones 1–6)

| Name | Version | License | Source | Used by | Redistributed | Obligations / restrictions |
|---|---|---|---|---|---|---|
| GoogleTest | 1.14.0 (system package) or 1.15.2 (FetchContent, pinned tag) | BSD-3-Clause | github.com/google/googletest | native tests | No | None for test-only use. |
| .NET 10 runtime + BCL | 10.0.x (SDK pinned via `global.json` 10.0.100, rollForward latestFeature) | MIT | dotnet.microsoft.com | all managed code | Yes (framework-dependent or self-contained) | Include .NET third-party notices if self-contained. |
| WPF (Microsoft.WindowsDesktop.App) | 10.0.x | MIT | github.com/dotnet/wpf | Desktop | Via .NET runtime | — |
| Microsoft.Extensions.Logging.Abstractions | 10.0.0 | MIT | nuget.org | Core | Yes | Keep copyright notice. |
| Microsoft.Extensions.Logging | 10.0.0 | MIT | nuget.org | Desktop | Yes | Keep copyright notice. |
| Microsoft.Extensions.Logging.Debug | 10.0.0 | MIT | nuget.org | Desktop | Yes | Keep copyright notice. |
| Microsoft.Extensions.DependencyInjection | 10.0.0 | MIT | nuget.org | Desktop | Yes | Keep copyright notice. |
| xunit | 2.9.3 | Apache-2.0 | nuget.org | tests | No | — |
| xunit.runner.visualstudio | 3.1.4 | Apache-2.0 | nuget.org | tests | No | — |
| Microsoft.NET.Test.Sdk | 17.14.1 | MIT | nuget.org | tests | No | — |

| FFmpeg (Windows builds, CI package) | n9.0.2-57-gd838f9f9ae, BtbN/FFmpeg-Builds release `autobuild-2026-11-16-15-29`, asset `ffmpeg-n9.0.2-57-gd838f9f9ae-win64-lgpl-shared-9.0.zip`, SHA-256 `bc661777e3aeb29cbd9e68f8b29a4ea1b54cc0db0373ff447085e5cb4ef307b3` | **LGPL-2.1-or-later** (verified at run time by the licence guard) | github.com/BtbN/FFmpeg-Builds | native engine (file inputs) | Yes — DLLs + `FFMPEG_LICENSE.txt` in the package | Dynamic linking only; ship licence text and the corresponding source (BtbN publishes build scripts; the exact FFmpeg source tag must be offered); users must be able to replace the DLLs. Record the full `configure` line from `ffmpeg -buildconf` before a commercial release. ⚖️ |
| Steinberg VST 3 interfaces (`vst3_pluginterfaces` only — no SDK helper code) | tag `v3.8.1_build_84`, commit `4f547e8e102b47de4a8b8aaf343c73b700786372` (FetchContent, pinned) | **MIT** (since VST 3.8.0) | github.com/steinbergmedia/vst3_pluginterfaces | native engine (VST3 host, M4); test plug-in | Yes — `base/funknown.cpp`, `base/coreiids.cpp` and the IIDs are compiled into `mixit_engine` | Ship the licence text (`VST3_PLUGINTERFACES_LICENSE.txt`, copied by `package-windows.ps1` and `cmake --install`). "VST" is a Steinberg trademark: name/logo use follows Steinberg's trademark guidelines (check before marketing material). Never use the VST2 SDK. ⚖️ (trademark only) |
| Windows platform APIs: Direct3D 11, DXGI, D3DCompiler (`d3dcompiler_47.dll`), Media Foundation | Windows 10/11 | part of Windows | Windows SDK | GPU compositor, capture | No (OS components) | `d3dcompiler_47.dll` is part of Windows 10/11 and is not shipped. |
| FreeType | 2.14.3, tag `VER-2-14-3`, commit `0a0221a1347e2f1e07c395263540026e9a0aa7c7` (FetchContent, pinned; static, all optional dependencies off) | **FreeType License (FTL)** — chosen over the GPL-2 option | gitlab.freedesktop.org/freetype (GitHub mirror) | native engine (graphics text, multiview labels, M6) | Yes — compiled into `mixit_engine` | FTL requires crediting "The FreeType Project" (www.freetype.org) in the documentation (About box / licence notices) and shipping the licence text (`FREETYPE_LICENSE.txt`, copied by `package-windows.ps1` and `cmake --install`). |
| DejaVu Sans / DejaVu Sans Bold fonts | 2.37, `dejavu-fonts-ttf-2.37.zip`, SHA-256 `7576310b219e04159d35ff61dd4a4ec4cdba4f35c00e002a136f00e96a908b0a` | Bitstream Vera Fonts licence + public-domain DejaVu changes (+ Arev licence for some glyphs) | dejavu-fonts.github.io | embedded in `mixit_engine` (default graphics font) | Yes — unmodified, embedded | Include the licence text with copies (`DEJAVU_FONTS_LICENSE.txt`); modified versions must not use the "Bitstream"/"Vera" names; the fonts may not be sold by themselves (bundling in software is allowed). |
| FFmpeg (dev host only) | Ubuntu 24.04 package 6.1.1-3ubuntu5 | **GPL-2.0-or-later** build | apt | native engine on Linux dev hosts | **No — never** | Allowed only because dev builds disable the licence policy; product builds reject it at runtime (ADR-0004). |

NuGet versions were pinned in an environment without nuget.org access; the first
restore on a connected machine must confirm they resolve (DEVELOPMENT_STATUS).

## Planned (not yet in any build)

| Name | Milestone | License | Constraints for a closed-source commercial product |
|---|---|---|---|
| OpenH264 (software H.264 fallback) | **in use since M5** — compiled into the pinned BtbN LGPL FFmpeg 9.0.2 build (not Cisco's binary) | BSD-2-Clause | Cisco's prebuilt binary carries their H.264 patent licence only when downloaded from Cisco at install/run time; **the copy inside the FFmpeg build does not**. Owner decision before M9: rely on hardware encoders, download Cisco's binary at run time, or license H.264. ⚖️ |
| H.264 / HEVC / AAC patents | **relevant since M5** (H.264 + AAC encoding) | patent pools | Separate from copyright; distributing encoders may require pool licences (H.264 and AAC: Via LA; HEVC: Access Advance and Via LA). Legal review before commercial release (M9). ⚖️ |
| NDI SDK | M3/M5 | NDI SDK License Agreement (royalty-free) | Attribution "NDI® is a registered trademark of Vizrt NDI AB" in About box and docs; link to ndi.video near NDI selection UI, website and docs; ship NDI DLLs in the app folder; EULA must cover NDI terms; AAC/H.264/H.265 licensing is the developer's responsibility. NDI Advanced SDK = separate commercial licence. |
| Blackmagic DeckLink SDK | M3/M5 | Blackmagic Design SDK EULA | Runtime via Desktop Video drivers (COM). Confirm header redistribution and trademark terms. ⚖️ |
| ASIO SDK | M4 | Steinberg proprietary licence **or** GPLv3 (dual, since Oct 2025) | Must use the **proprietary** agreement; the GPLv3 option is incompatible with closed source. ASIO trademark/logo rules. ⚖️ |
| libsrt (SRT) | M5 | MPL-2.0 | Dynamic linking; publish modifications to MPL-covered files. |
| SQLite via Microsoft.Data.Sqlite / SQLitePCLRaw | M2/M8 | Public domain / MIT / Apache-2.0 | Keep notices. |
| HTML renderer: CEF **or** WebView2 | deferred after M6 (ADR-0011) | BSD-3 (CEF; Chromium bundles many licences) / Microsoft WebView2 licence | Decide in its own ADR; CEF requires shipping Chromium's notices file. |
| Google Benchmark | M2 | Apache-2.0 | Test-only. |

## Attribution checklist for the About box (to be completed by M9)

- [ ] FFmpeg LGPL notice + source location
- [ ] NDI® trademark attribution + ndi.video link (if NDI shipped)
- [ ] VST® / ASIO® trademark notices (if shipped)
- [ ] .NET / Microsoft.Extensions MIT notices
- [ ] Any additional components added above
