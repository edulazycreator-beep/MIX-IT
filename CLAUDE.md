# CLAUDE.md

# MIX IT --- PROJECT CONTEXT

## 1. Project Identity

-   **Project name:** Mix It
-   **Product type:** Professional Live Production & Broadcast Studio
-   **Platform:** Windows Desktop
-   **Target OS:** Windows 10/11 x64
-   **Architecture:** Hybrid C# + Native C++
-   **Development environment:** Claude Code
-   **Development strategy:** Core Engine First
-   **Product model:** Free + Professional License
-   **Source code policy:** Original implementation only

Mix It is an independent professional broadcast production software
project. Its purpose is to provide a complete environment for
multi-camera live production, video switching, audio mixing, graphics,
streaming, recording, and broadcast automation.

Target use cases include:

-   Wedding live streaming
-   Corporate events and conferences
-   Multi-camera production
-   Television-style production
-   Religious events
-   Concerts and stage productions
-   Online broadcasting
-   LED screen and external display production

## 2. Project Vision

Build Mix It into a professional broadcast production platform with
capabilities comparable to established industry solutions such as vMix,
while maintaining its own original:

-   Source code
-   Software architecture
-   User interface
-   Media processing engine
-   Production workflow
-   Product identity

Do not clone, decompile, reverse-engineer, or copy proprietary
implementations from vMix or other commercial applications. Use publicly
documented technologies, standards, and legally available SDKs.

The ultimate goal is to create an independent, maintainable, scalable,
and commercially viable broadcast software ecosystem.

## 3. Core Development Philosophy

**BUILD THE ENGINE FIRST, THEN BUILD THE PRODUCTION EXPERIENCE.**

Development priorities:

1.  Architecture and project foundation
2.  Native media engine
3.  Video processing engine
4.  Audio processing engine
5.  Video compositing and switching
6.  Streaming and recording
7.  Graphics and production tools
8.  Desktop interface
9.  Advanced automation
10. Optimization and commercial release

Do not prioritize visual appearance over functional engineering. Every
feature must have a working implementation or be explicitly marked as
planned, experimental, or incomplete. Never represent mock functionality
as production-ready functionality.

## 4. Technology Stack

### Desktop application

-   C#
-   .NET 10 LTS
-   WPF
-   MVVM
-   Dependency injection
-   Async/await

### Native engine

-   C++20 or newer where supported
-   MSVC
-   CMake
-   Native C ABI
-   Carefully managed P/Invoke interoperability

### Video and media

-   FFmpeg libraries
-   Windows Media Foundation
-   DirectShow as a compatibility fallback
-   Blackmagic DeckLink SDK
-   NDI SDK
-   RTSP
-   SRT

### GPU

-   Direct3D 11
-   Direct3D 12 where appropriate
-   GPU compositing
-   Hardware encoding

### Audio

-   WASAPI
-   ASIO SDK
-   VST3 SDK
-   Native DSP

### Data and testing

-   SQLite
-   JSON
-   GoogleTest
-   .NET test framework
-   Integration testing
-   Performance testing

All dependencies must be compatible with the project's licensing and
commercial distribution requirements. Pin dependency versions and
document redistribution obligations. Treat optional vendor SDKs as
optional integrations rather than mandatory build dependencies.

## 5. System Architecture

Mix It must use independent and modular engines.

### MixIt.Desktop

-   User interface
-   Workspace
-   Project management
-   Configuration
-   Production controls

### MixIt.Native

-   Native C++ engine
-   Media processing
-   GPU resources
-   Audio processing
-   Video processing

### MixIt.Core

-   Application services
-   Engine management
-   Input management
-   Output management
-   Production workflow

### MixIt.SDK

-   External integration interfaces
-   Plugin interfaces
-   Extension interfaces

### MixIt.Protocol

-   Internal communication contracts
-   Remote control API
-   Event definitions

### MixIt.Licensing

-   Free license
-   Professional license
-   Activation
-   License validation

Keep UI logic separate from media processing. Use a stable, versioned
C-compatible ABI between managed and native code. Do not expose unstable
C++ classes directly through P/Invoke.

## 6. Core Product Features

### Video engine

-   Scalable video inputs
-   HDMI capture
-   USB capture
-   Webcam
-   NDI
-   IP camera
-   RTSP
-   SRT
-   Video files
-   Images
-   Screen capture
-   Preview and Program buses
-   Video switching and transitions
-   Multi-layer compositing
-   Chroma key and luma key
-   Color correction
-   Picture-in-picture
-   Video delay

### Audio engine

-   Scalable audio channels
-   Audio mixing and gain
-   Volume, pan, mute, and solo
-   EQ
-   Compressor
-   Limiter
-   Noise gate and expander
-   Audio routing and auxiliary buses
-   Master output
-   Audio monitoring
-   VST3 plugin hosting
-   Stereo and multichannel support

### Streaming engine

-   RTMP
-   RTMPS
-   SRT
-   HLS
-   Multi-destination streaming
-   Hardware encoding
-   Network monitoring
-   Automatic reconnection
-   Streaming profiles

### Recording engine

-   Program recording
-   ISO recording
-   Multi-session recording
-   Configurable codecs and bitrate
-   MP4, MOV, and MKV where supported by selected codecs and muxers
-   Recording profiles
-   Storage monitoring
-   Segmentation and recovery strategies

### Graphics engine

-   Lower thirds
-   Titles
-   Logo overlays
-   Text and PNG overlays
-   Countdown and clock
-   Scoreboard
-   Animated graphics
-   HTML/browser graphics
-   Multiple layers

### Advanced production

-   Multiview
-   Virtual sets
-   Instant replay
-   Slow motion
-   Macros
-   Automation
-   PTZ camera control
-   Tally
-   External display output
-   Clean feed
-   Remote control API

These are product capabilities and roadmap targets, not claims that
every feature already exists.

## 7. Original UI/UX Design

Mix It must have its own visual identity.

Design principles:

-   Modern and professional
-   Minimal dark studio appearance
-   High information density
-   Clear visual hierarchy
-   Operator-focused workflow
-   Customizable workspace
-   Efficient access to production controls

Do not directly reproduce the layout, artwork, icons, or appearance of
vMix or another commercial application.

Main application sections:

1.  Production Studio
2.  Input Manager
3.  Audio Mixer
4.  Streaming Control
5.  Recording Control
6.  Graphics Studio
7.  Replay Studio
8.  Virtual Studio
9.  Automation
10. Hardware Diagnostics
11. Settings
12. License Management

Use reusable components and MVVM correctly. Do not put media processing
logic inside ViewModels or UI event handlers.

## 8. Scalability and Hardware Management

Use dynamic resource allocation. Do not hardcode arbitrary input or
output limits into the architecture. However, actual processing capacity
must respect available hardware resources; never promise literally
unlimited processing.

Implement detection and monitoring for:

-   CPU
-   GPU
-   RAM
-   GPU memory where exposed
-   Capture devices
-   Audio devices
-   Hardware encoders
-   Network interfaces
-   Storage devices

Performance profiles:

-   Low Resource
-   Balanced
-   High Performance
-   Professional

Support graceful degradation when resources become insufficient. Do not
automatically change critical production settings during an active
broadcast without user approval.

## 9. Code Quality Rules

1.  Write clean, original, maintainable code.
2.  Use descriptive names.
3.  Avoid duplicated logic.
4.  Avoid unnecessary abstractions and dependencies.
5.  Separate responsibilities.
6.  Keep modules independently testable.
7.  Handle errors explicitly.
8.  Release native resources correctly.
9.  Avoid memory leaks and deadlocks.
10. Never block real-time audio callbacks with disk, network, UI, or
    other blocking operations.
11. Use bounded queues and explicit backpressure policies.
12. Avoid unnecessary CPU/GPU transfers.
13. Keep media processing independent from the UI.
14. Document important APIs and ownership rules.
15. Preserve working functionality when modifying existing code.

Do not use placeholder implementations unless explicitly identified as
development-only.

## 10. Claude Code Development Rules

Before implementing any feature:

-   Inspect the existing repository.
-   Read relevant documentation.
-   Understand the current architecture.
-   Identify dependencies and integration requirements.
-   Check existing tests and build instructions.

During development:

-   Work incrementally.
-   Avoid rewriting working modules unnecessarily.
-   Implement one milestone at a time.
-   Build after meaningful changes.
-   Run relevant tests.
-   Fix compilation and test failures.
-   Update documentation.
-   Report actual implementation status.

Never claim that a feature is complete without verification. Never hide
build errors or failed tests.

If an SDK is unavailable, create a documented adapter interface and a
testable development substitute where appropriate. Clearly mark real
hardware integration as pending. Do not invent SDK APIs.

## 11. Development Milestones

### Milestone 1 --- Foundation

-   Repository setup
-   CMake configuration
-   .NET solution
-   Native engine library
-   C ABI
-   C# interoperability
-   Logging
-   Error handling
-   Initial tests

### Milestone 2 --- Media Pipeline Foundation

-   FFmpeg integration
-   Media abstraction
-   Synthetic video source
-   Synthetic audio source
-   Frame and audio buffers
-   Timestamp handling
-   Bounded media queues

### Milestone 3 --- Video Engine

-   Video decoding
-   Video capture abstraction
-   GPU rendering
-   Preview
-   Program
-   Switching
-   Compositing

### Milestone 4 --- Audio Engine

-   Audio capture
-   Mixer
-   Routing
-   DSP
-   Audio monitoring
-   Synchronization

### Milestone 5 --- Streaming and Recording

-   RTMP and RTMPS
-   SRT
-   HLS
-   Hardware encoding
-   Multi-destination streaming
-   ISO recording

### Milestone 6 --- Graphics and Production

-   Titles
-   Overlays
-   Chroma key
-   Virtual sets
-   Multiview
-   Replay

### Milestone 7 --- Advanced Features

-   Macros
-   Automation
-   PTZ
-   Tally
-   Remote API
-   Plugin system

### Milestone 8 --- Desktop Experience

-   Final custom interface
-   Workspace customization
-   Production dashboard
-   User profiles
-   Hardware diagnostics

### Milestone 9 --- Commercial Release

-   Free and Professional editions
-   Licensing
-   Installer
-   Compatibility testing
-   Performance optimization
-   Documentation

## 12. Documentation Requirements

Maintain these files and update them when implementation or architecture
changes:

-   `PROJECT_OBJECTIVE.md`
-   `PRODUCT_SPEC.md`
-   `ARCHITECTURE.md`
-   `ENGINE_DESIGN.md`
-   `API_CONTRACT.md`
-   `DEVELOPMENT_STATUS.md`
-   `ROADMAP.md`
-   `BUILD_GUIDE.md`
-   `TEST_REPORT.md`
-   `THIRD_PARTY_LICENSES.md`
-   `CHANGELOG.md`

Documentation must reflect actual code and verified status. Do not
present planned features as completed features.

## 13. Definition of Done

A feature is complete only when:

-   Source code is implemented.
-   The project compiles.
-   Relevant tests pass.
-   Error handling exists.
-   Resource management is verified.
-   Integration works.
-   Documentation is updated.
-   Actual functionality has been validated.

A visible UI element alone does not qualify as an implemented feature. A
simulated video output does not qualify as a real broadcast output.

## 14. Current Project Status

The project is in the initial architecture and implementation stage.

**Current priority: MILESTONE 1 --- FOUNDATION.**

Do not jump directly into advanced production features.

First establish a stable, testable, extensible core architecture.

Maintain `DEVELOPMENT_STATUS.md` with:

-   Completed tasks
-   In-progress tasks
-   Pending tasks
-   Build status
-   Test results
-   Known issues
-   Next development objectives

At the beginning of each new session, read this file and inspect the
repository before continuing.

## 15. Final Project Mission

Build Mix It into a genuinely professional, reliable, scalable, and
commercially distributable broadcast production platform.

Always prioritize:

1.  Engineering quality
2.  Stability
3.  Real-time performance
4.  Audio/video synchronization
5.  Hardware compatibility
6.  Modular architecture
7.  Maintainability
8.  Operator usability
9.  Commercial readiness

The ultimate goal is to establish Mix It as an independent professional
broadcast software platform with original technology and its own
identity.

**BUILD ORIGINAL. BUILD MODULAR. BUILD RELIABLE. BUILD PROFESSIONAL.**

-   **Project:** MIX IT
-   **Product:** Professional Live Production & Broadcast Studio
-   **Platform:** Windows 10/11 x64
-   **Architecture:** C# + Native C++
-   **Strategy:** Core Engine First
-   **Source code:** Original implementation only

# 16. ADDITIONAL ENGINEERING RULES

## A. Architecture Protection

- Never introduce architectural changes without understanding their impact.
- Do not create unnecessary dependencies between modules.
- Keep video, audio, streaming, recording, and UI responsibilities separated.
- All modules must communicate through documented interfaces.
- Avoid circular dependencies.
- Avoid tightly coupled components.
- Prefer dependency injection and explicit ownership.
- Document major architectural decisions in an Architecture Decision Record (ADR).
- Do not rewrite the core engine without a verified technical reason.

## B. Code Quality and Refactoring

- Write the smallest maintainable implementation that satisfies the requirement.
- Avoid duplicated code.
- Remove genuinely unused code after verifying it has no runtime or integration purpose.
- Avoid unnecessary wrapper classes and abstractions.
- Use consistent naming conventions.
- Keep functions focused and reasonably sized.
- Refactor incrementally.
- Never sacrifice reliability for fewer lines of code.
- Never remove error handling merely to simplify implementation.
- Preserve backward compatibility where practical.

## C. Build and Test Enforcement

After every significant implementation:

1. Compile the affected module.
2. Run relevant unit tests.
3. Run integration tests where applicable.
4. Check compiler warnings.
5. Check runtime logs.
6. Verify resource cleanup.
7. Update test documentation.

Never claim that a build or test succeeded unless it was actually executed.

Never ignore compiler errors or silently suppress important warnings.

## D. Performance Engineering

- Profile before optimizing.
- Avoid unnecessary memory allocation in real-time pipelines.
- Use bounded queues.
- Avoid blocking operations in critical processing threads.
- Minimize CPU/GPU data transfers.
- Reuse buffers and GPU resources where appropriate.
- Monitor processing latency.
- Monitor frame drops.
- Monitor audio buffer underruns.
- Implement overload handling.
- Avoid uncontrolled memory growth.
- Benchmark performance against documented hardware configurations.

## E. Audio/Video Synchronization

- Use explicit timestamps.
- Establish a clear clock synchronization strategy.
- Define audio and video time bases.
- Handle variable frame rates correctly.
- Handle audio sample-rate conversion.
- Avoid accumulating synchronization drift.
- Define buffering and latency policies.
- Handle dropped and late frames.
- Test long-duration playback and streaming stability.

Audio/video synchronization must be treated as a fundamental architectural requirement.

## F. Security and Licensing

- Never hardcode API keys, passwords, or license secrets.
- Protect streaming credentials.
- Validate external media inputs.
- Maintain dependency and license inventories.
- Respect third-party SDK licensing.
- Review FFmpeg licensing implications before commercial distribution.
- Keep licensing implementation modular.
- Do not introduce dependencies with incompatible redistribution terms.

## G. UI/UX Consistency

- Maintain a consistent design system.
- Use reusable controls.
- Use centralized themes and styles.
- Maintain consistent spacing, typography, and colors.
- Support different screen resolutions.
- Support Windows display scaling.
- Avoid freezing the UI during media processing.
- Provide clear error messages.
- Show meaningful loading, connection, and recording states.
- Never display false operational status.

## H. Session Continuity

At the beginning of every Claude Code session:

1. Read CLAUDE.md.
2. Read DEVELOPMENT_STATUS.md.
3. Read ROADMAP.md.
4. Inspect the current repository.
5. Check the latest build and test reports.
6. Identify the last completed milestone.
7. Continue from the verified project state.

At the end of every development session:

1. Update DEVELOPMENT_STATUS.md.
2. Update ROADMAP.md if necessary.
3. Record completed work.
4. Record incomplete work.
5. Record known issues.
6. Record build and test results.
7. Identify the next recommended task.

Never assume that an unfinished task has been completed.

## I. Feature Integrity

Every feature must have a documented implementation status:

- PLANNED
- IN_PROGRESS
- EXPERIMENTAL
- IMPLEMENTED
- TESTED
- PRODUCTION_READY

A feature must never be marked PRODUCTION_READY without actual integration and validation.

## J. Engineering Decision Policy

When multiple implementation approaches are available:

1. Evaluate performance.
2. Evaluate stability.
3. Evaluate maintainability.
4. Evaluate licensing.
5. Evaluate hardware compatibility.
6. Evaluate long-term scalability.

Choose the approach that provides the best overall engineering outcome.

Document important decisions and their trade-offs.

Do not introduce a new framework or major dependency without a clear technical justification.

## K. Production Safety

Mix It is intended for live broadcast environments.

Prioritize operational reliability.

- Do not allow an individual streaming destination to block the entire production pipeline.
- Do not allow UI failures to unnecessarily terminate media processing.
- Implement graceful shutdown.
- Protect active recording sessions.
- Handle device disconnection.
- Handle network interruption.
- Provide clear recovery procedures.
- Never silently discard production data.
- Clearly communicate degraded operation to the operator.

**FINAL RULE: Reliability, synchronization, and production stability take priority over adding more features.**
