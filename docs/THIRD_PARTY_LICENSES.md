# Third-Party Dependency Inventory

Every third-party component must be listed here **before** it is added to a
build. "Redistributed" means it ships with the product. Not legal advice;
⚖️ marks items requiring review by qualified counsel before commercial release.

## In use (Milestones 1–3)

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
| Windows platform APIs: Direct3D 11, DXGI, D3DCompiler (`d3dcompiler_47.dll`), Media Foundation | Windows 10/11 | part of Windows | Windows SDK | GPU compositor, capture | No (OS components) | `d3dcompiler_47.dll` is part of Windows 10/11 and is not shipped. |
| FFmpeg (dev host only) | Ubuntu 24.04 package 6.1.1-3ubuntu5 | **GPL-2.0-or-later** build | apt | native engine on Linux dev hosts | **No — never** | Allowed only because dev builds disable the licence policy; product builds reject it at runtime (ADR-0004). |

NuGet versions were pinned in an environment without nuget.org access; the first
restore on a connected machine must confirm they resolve (DEVELOPMENT_STATUS).

## Planned (not yet in any build)

| Name | Milestone | License | Constraints for a closed-source commercial product |
|---|---|---|---|
| OpenH264 (optional software H.264) | M5 | BSD-2-Clause | Cisco's prebuilt binary carries their H.264 patent licence only when downloaded from Cisco at install/run time; self-built binaries do not. ⚖️ |
| H.264 / HEVC / AAC patents | M5 | patent pools | Separate from copyright; encoder distribution may require pool licences. ⚖️ |
| NDI SDK | M3/M5 | NDI SDK License Agreement (royalty-free) | Attribution "NDI® is a registered trademark of Vizrt NDI AB" in About box and docs; link to ndi.video near NDI selection UI, website and docs; ship NDI DLLs in the app folder; EULA must cover NDI terms; AAC/H.264/H.265 licensing is the developer's responsibility. NDI Advanced SDK = separate commercial licence. |
| Blackmagic DeckLink SDK | M3/M5 | Blackmagic Design SDK EULA | Runtime via Desktop Video drivers (COM). Confirm header redistribution and trademark terms. ⚖️ |
| ASIO SDK | M4 | Steinberg proprietary licence **or** GPLv3 (dual, since Oct 2025) | Must use the **proprietary** agreement; the GPLv3 option is incompatible with closed source. ASIO trademark/logo rules. ⚖️ |
| VST 3 SDK | M4 | MIT (from VST 3.8.0, Oct 2025) | Retain copyright + licence text. VST trademark/logo usage governed separately. Do not use the VST2 SDK. |
| libsrt (SRT) | M5 | MPL-2.0 | Dynamic linking; publish modifications to MPL-covered files. |
| SQLite via Microsoft.Data.Sqlite / SQLitePCLRaw | M2/M8 | Public domain / MIT / Apache-2.0 | Keep notices. |
| HTML renderer: CEF **or** WebView2 | M6 | BSD-3 (CEF; Chromium bundles many licences) / Microsoft WebView2 licence | Decide in an M6 ADR; CEF requires shipping Chromium's notices file. |
| Google Benchmark | M2 | Apache-2.0 | Test-only. |

## Attribution checklist for the About box (to be completed by M9)

- [ ] FFmpeg LGPL notice + source location
- [ ] NDI® trademark attribution + ndi.video link (if NDI shipped)
- [ ] VST® / ASIO® trademark notices (if shipped)
- [ ] .NET / Microsoft.Extensions MIT notices
- [ ] Any additional components added above
