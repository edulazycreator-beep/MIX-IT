# MIX IT — Remote Control API (protocol 1.0)

Status: **IMPLEMENTED** (M7). Tested with automated HTTP/WebSocket clients;
no third-party control surface has been tested yet. Contracts live in
`managed/MixIt.Protocol` (C# records, JSON camelCase, enums as camelCase strings).

## Enabling

MIX IT → **Automation & Control → Remote control**: enable, choose the port
(default **8099**), and copy the **token**. By default the server listens on
this PC only (`localhost`). "Allow other computers on the network" listens on
every interface (Windows asks for a firewall exception). Use it only on a
trusted network: the transport is plain HTTP.

Opening `http://<pc>:<port>/` in a browser shows a small control page
(inputs with tally, CUT, AUTO, overlay toggles, macro buttons). It asks for the
token and keeps it for the browser tab only.

## Authentication

Every request except `GET /` needs `Authorization: Bearer <token>`.
WebSocket clients that cannot set headers (browsers) may pass `?token=<token>`
on `/api/v1/events` only. A wrong or missing token gets `401` with
`{"ok":false,"error":"missing or wrong token"}`. Generating a new token in the
app locks out every client using the old one.

## Endpoints (base `/api/v1`)

| Method & path | Answer |
|---|---|
| `GET /info` | `{"product":"MIX IT","protocol":"1.0","sdk":"0.1"}` |
| `GET /state` | `ProductionState`: `engineAvailable`, `running`, `format`, `programInputId`, `previewInputId`, `inputs[]` (`id`, `number`, `name`, `kind`), `outputs[]` (`id`, `name`, `kind`, `state`, `address` with keys/passwords redacted), `tally[]` |
| `GET /tally` | `TallyEntry[]`: `inputId`, `number`, `name`, `program`, `preview` |
| `GET /commands` | `CommandDescriptor[]`: `name`, `description`, `category`, `parameters[]` (`name`, `kind`, `required`, `description`) |
| `GET /macros` | `[{"name":…, "steps":…}]` |
| `POST /commands/{name}` | runs a command. Body `{"args":{…}}` or a flat object; values may be strings, numbers or booleans. `200` + `CommandResult` (`ok`, `error`, `value`) on success, `400` on a command error or bad JSON, `404` for an unknown command |
| `POST /macros/{name}/run` | starts a macro (returns when started) |
| `GET /events` (WebSocket) | event stream, see below |

Request bodies are limited to 64 KiB.

## Event stream

Each message is a `ServerEvent`: `{"type": …, "time": …, "data": …}`.

| type | data |
|---|---|
| `state` | `ProductionState`, sent once on connect |
| `tally` | `TallyEntry[]` whenever any input's tally changes |
| `command` | `{"name","ok","error"}` after every command from any source |
| `macro` | `MacroRun` (`macro`, `stepsRun`, `stepCount`, `cancelled`, `error`) when a macro ends |
| `result` | `{"id", "result"}` answer to a command sent over the socket |

Clients may send commands over the socket:
`{"command":"Cut","args":{},"id":"42"}` (messages ≤ 64 KiB). A client that
reads too slowly loses its oldest events (queue of 256 per client); other
clients and the production are not affected. At most 32 event clients.

## Commands

Inputs are addressed by **number** (`1` = first input in the list), by engine
id (`id:12`) or by **name** (case-insensitive). Outputs by id or name.
`GET /commands` is the authoritative list (extensions add their own, prefixed).

| Category | Commands |
|---|---|
| Production | `Cut`, `Auto kind=mix\|fade\|wipe\|cut frames=30`, `SetPreview input=`, `SetProgram input=`, `PictureInPicture input=\|none`, `StartProduction`, `StopProduction` |
| Overlays | `OverlayIn channel=1..4 fade=15`, `OverlayOut`, `OverlayToggle`, `OverlaySelect channel= input=\|none` |
| Graphics | `SetText input= field= value=`, `TimerStart input=`, `TimerPause input=`, `TimerSet input= mode=countdown\|stopwatch seconds=` |
| Audio | `SetVolume input= db=-96..12`, `Mute input= value=true\|false\|toggle` |
| Outputs | `StartOutput output=`, `StopOutput output=`, `StartRecordings`, `StopRecordings` |
| Replay | `ReplayMark input=`, `ReplayPlay input= speed=0.5 from=mark\|last seconds=5`, `ReplayPause input= value=`, `ReplaySpeed input= speed=` |
| Macros | `RunMacro name= wait=false`, `StopMacros`, `Wait ms=` |
| PTZ | `PtzPreset camera= preset=`, `PtzStorePreset`, `PtzMove camera= direction= speed=`, `PtzStop`, `PtzZoom camera= direction=in\|out\|stop speed=`, `PtzHome` |

## Example

```sh
TOKEN=...   # from the app
curl -H "Authorization: Bearer $TOKEN" http://localhost:8099/api/v1/state
curl -X POST -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
     -d '{"args":{"input":"Camera 2"}}' http://localhost:8099/api/v1/commands/SetPreview
curl -X POST -H "Authorization: Bearer $TOKEN" http://localhost:8099/api/v1/commands/Cut
```

## Versioning

`protocol` follows major.minor: new commands, fields and event types are
minor changes (clients must ignore unknown fields); removals or meaning
changes bump the major version and the base path (`/api/v2`).
