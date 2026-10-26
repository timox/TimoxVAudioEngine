# TimoxVasio local control API v1

[Français](API.md) | **English**

This API is the contract between the control application and the native engine. Electron main is its client; the renderer reads neither the registry, configuration file, nor shared audio memory. For an introduction with diagrams and PowerShell/Node.js examples, see the [API quick start](API_QUICKSTART.en.md). The normative machine-readable contract is [OpenAPI 3.1](../api/openapi-v1.json), including its `x-websocket` extension, and the [shared JSON schemas](../api/schemas/api-v1.json). The [feature guide](FONCTIONS.en.md) illustrates measurements and the integrated console.

The native server uses `cpp-httplib` 0.58.0 for HTTP/WebSocket and `nlohmann/json` 3.12.0 for JSON. Dependency notices and licenses are retained in `vendor/`. The physical inventory enumerates registered ASIO drivers without opening them. Until the engine explicitly selects and opens one, `capabilitiesKnown` is `false` and its capabilities are empty.

Messages use UTF-8 JSON. Units are explicit: sample rate in hertz, buffer size in frames, channel numbers starting at 1, gain in dB, and peak level in dBFS.

## Address and reads

The engine listens only on loopback `127.0.0.1`. A startup port can be specified; `0` requests a free port, which the client obtains from the engine startup information.

Control from Linux is not available: no remote API access is open. Certificate
pairing and the TLS gateway described in the
[remote-control proposal](superpowers/specs/2026-10-24-remote-timox-api-pairing.md)
is not implemented.

| Request | Response |
| --- | --- |
| `GET /api/v1/state` | Confirmed engine state, physical selection, TimoxVasio configuration, routes, inventories, active stereo pairs |
| `GET /api/v1/drivers` | Discovered physical ASIO drivers, the virtual TimoxVasio driver, and measurable stereo pairs |
| `GET /api/v1/openapi.json` | Locally served OpenAPI document for Swagger UI |
| `GET /api/v1/schemas/api-v1.json` | Shared JSON schema referenced by OpenAPI |
| `GET /api/v1/application-profiles` | ASIO channel-capacity profiles by executable name |
| `PUT /api/v1/application-profiles` | Complete, persistent replacement of profiles |
| `GET /api/v1/pwar` | PWAR network settings saved in `pwarASIO.cfg` |
| `PUT /api/v1/pwar` | Saves the Windows PWAR destination address and port |
| `ws://127.0.0.1:<port>/api/v1/ws` | Configuration commands and state/audio events |

These two routes manage only saved Windows network settings. `PUT` returns
`reopenRequired`: restart the PWAR Windows client to load the new settings.
Timox does not start that client, and these routes do not report its ASIO
connection, network activity or audio signal. Once the client opens
`TimoxVasio.dll`, it appears in `virtualDrivers[].clients` like any other ASIO
application. In the graph, route a source to its inputs to send audio to Linux;
route its outputs to a destination to receive audio from Linux. The local
physical ASIO device remains selected independently.

HTTP requests use `http://127.0.0.1:<port>`. Responses conform to `definitions.state` or `definitions.drivers`. A physical driver reports input/output ports, reported sample rates, and buffer capabilities after opening. TimoxVasio reports processes directly attached through ASIO and the channels each allocated through `createBuffers`. A client remains listed if its `inputChannels` and `outputChannels` arrays are empty: it opened the driver but has not created routable ports yet. Direct membership alone does not prove that its buffers contain audible signal.

Endpoint ID forms:

```text
physical:<driverId>:input:<channel>
physical:<driverId>:output:<channel>
virtual:TimoxVasio:<client-pid>:input:<channel>
virtual:TimoxVasio:<client-pid>:output:<channel>
```

The channel suffix is a decimal number starting at 1. Reuse IDs returned by the engine verbatim. `GET /api/v1/state` distinguishes active `routes` from all saved `configuredRoutes`. A route for an absent application stays configured with a stable ID such as `virtual:TimoxVasio:app:<executable>:output:<channel>` or its `input` form, and activates when that application opens its channels again, even with a different PID. A new stable ID is accepted when the executable has an `application-profiles` entry and the channel lies within its advertised capacity. If several running instances share an executable name, the route waits for an unambiguous match. Old PID-only routes cannot be attributed to another application after that process exits; they remain saved for explicit correction.

## Per-application capacity profiles

`GET /api/v1/application-profiles` returns the complete explicitly configured list:

```json
{
  "profiles": [
    { "processName": "mixxx.exe", "inputChannels": 255, "outputChannels": 255 }
  ]
}
```

`processName` must be a Windows executable filename without a path; matching is case-insensitive. An unlisted application receives 256 inputs and 256 outputs. The initial Mixxx profile is 255/255 because the target stable version interprets a capacity of 256 as zero. The limit applies to `getChannels`, `getChannelInfo`, and `createBuffers`, not to the 256-slot shared transport.

`PUT /api/v1/application-profiles` replaces the entire list:

```json
{
  "profiles": [
    { "processName": "mixxx.exe", "inputChannels": 255, "outputChannels": 255 },
    { "processName": "another-host.exe", "inputChannels": 128, "outputChannels": 64 }
  ]
}
```

Counts are inclusive from 1 to 256. An empty list clears overrides and restores the 256/256 default. The response confirms saved profiles and lists affected connected clients in `restartRequiredClients`. An initialized host retains its existing `getChannels` result until manually closed and restarted; the API does not restart it. An invalid request returns HTTP 400 with `INVALID_APPLICATION_PROFILES`. Storage is atomic; if saving fails, the previous active profiles remain.

After a physical driver is explicitly opened, endpoint `name` uses `IASIO::getChannelInfo` when usable, otherwise a fallback assembled from driver name, direction, and channel number. Before opening, physical endpoints and capabilities are empty.

## WebSocket commands

Each command is JSON:

```json
{
  "id": "request-42",
  "command": "configuration.apply",
  "payload": {
    "physicalDriverId": "physical-driver-stable-id",
    "sampleRate": 48000,
    "bufferFrames": 256,
    "routes": [{
      "id": "route-1",
      "sourceEndpointId": "virtual:TimoxVasio:1234:output:1",
      "destinationEndpointId": "physical:physical-driver-stable-id:output:1",
      "gainDb": 0.0,
      "mute": false
    }]
  }
}
```

`id` is a nonempty 1–128-character string unique among pending commands on the connection. `configuration.apply` replaces the physical driver, sample rate, buffer size, and entire route list. The whole object is validated before mutation. Repeating the command does not duplicate routes, but reconfiguration may briefly interrupt audio.

`GET /api/v1/state` exposes `configurationRevision`, incremented after each saved configuration. A client editing a state copy can send `expectedConfigurationRevision` in the `configuration.apply` payload. If another application applied a configuration in the meantime, the API returns `CONFIGURATION_CONFLICT` before changing audio. Clients omitting the field keep the historical full-replacement behavior and must refresh state to preserve other applications' routes.

`sampleRate` and `bufferFrames` are values for the chosen physical driver. `null` requests its current rate and preferred size, respectively. The engine sets the physical rate, re-reads rate-dependent capabilities, and refuses an unadvertised size. TimoxVasio receives these exact values; it has no independent rate or size setting.

`GET /api/v1/state` and `GET /api/v1/drivers` include `stereoPairs`: adjacent L/R output channels of one client, both open and routed. `audio.correlation.start` selects one by `stereoPairId`; `audio.correlation.stop` ends measurement. Selection is global to the engine; only one pair is measured at a time.

`{ "id": "start-1", "command": "engine.start" }` and `{ "id": "stop-1", "command": "engine.stop" }` need no payload. `engine.start` reapplies the last confirmed configuration and is idempotent; the engine stays stopped if no route is configured. `engine.stop` stops audio, closes the physical driver, and retains configuration for a later `engine.start`. It is rejected with `ENGINE_CLIENTS_CONNECTED` while a TimoxVasio ASIO client is connected. Neither command terminates the host process or HTTP/WebSocket server. An external program can therefore control the engine once Control or a supervisor has started the API host; the API cannot launch a nonexistent host process.

`{ "id": "reset-1", "command": "engine.reset" }` reopens the physical driver with the last saved configuration even while audio is running. It briefly interrupts audio, preserves saved connections, and does not apply pending UI edits.

`{ "id": "measure-1", "command": "performance.measurements.set", "payload": { "enabled": true } }` enables stage timing; `false` stops it and clears the previous window. The command is idempotent. Measurements are off each time the engine starts. `GET /api/v1/state` always exposes `performanceMeasurements` with `enabled`, `sampleEveryCallbacks: 16` and `window`. The window is `null` before five complete seconds or while collection is off. A complete window carries `durationMs: 5000`, `clients` and `physical`. Each client, identified by `processId` and `driverId`, exposes `hostCallback`, `ringWrite`, `ringWait` and `ringWaitDiscardedBlocks`; the physical side exposes `clientRead`, `routing` and `outputCopy`. Each duration is `null` without observation, or holds `samples`, `meanNs` and `worstNs` in nanoseconds. Ring wait measures publication until a whole matching block is read; partial reads are discarded. The physical buffer handoff does not measure hardware or acoustic latency. This collection adjusts neither buffer nor phase.

Each `virtualDrivers[].clients` entry in `/api/v1/state` and `/api/v1/drivers` also exposes `processCpuPercent` and `processCpuWindowMs`. CPU percentage is Windows process CPU time divided by elapsed time and the number of active logical processors. It is `null` until a second reading is available or when the process cannot be queried; the window is then also `null`. Measurement runs on API requests, outside the audio callback, and cannot break CPU usage down by hosted VST.

Success is acknowledged under the same `id`:

```json
{ "id": "request-42", "success": true, "result": { "accepted": true } }
```

Failure returns a structured error:

```json
{
  "id": "request-42",
  "success": false,
  "error": {
    "code": "UNSUPPORTED_SAMPLE_RATE",
    "message": "The physical driver does not support 88200 Hz",
    "operation": "open",
    "driverId": "physical-driver-stable-id",
    "asioError": -998
  }
}
```

Codes are stable; messages can be localized:

| Code | Meaning |
| --- | --- |
| `INVALID_CONFIGURATION` | Missing fields or malformed configuration |
| `UNKNOWN_ENDPOINT` | Endpoint absent from the current inventory |
| `INVALID_ROUTE_DIRECTION` | Route is not one of the three allowed types |
| `UNSUPPORTED_SAMPLE_RATE` | Selected physical driver does not accept the rate |
| `UNSUPPORTED_BUFFER_SIZE` | Driver refuses the requested frame count |
| `PHYSICAL_DRIVER_OPEN_FAILED` | Physical driver cannot be opened or started |
| `AUDIO_ENGINE_UNAVAILABLE` | Engine or a VASIO client transport is unavailable |
| `INTERNAL_ERROR` | Unclassified error while applying configuration |

## Reconfiguration interruption

An effective change stops audio before destroying or rebuilding buffers and the graph. The observable sequence is: (1) command response `accepted`; (2) `engine.status` with `state: "reconfiguring"`; (3) `engine.status` with `state: "stopped"` if the physical driver is configured but no route exists, with capabilities available through `/api/v1/drivers`; (4) `engine.status` with `state: "running"` if a route exists and validation, opening, buffer creation, and start succeed; otherwise (5) `engine.error` followed by `engine.status` with `state: "error"`. On error, the engine remains stopped and does not restore the old graph.

Even an identical active configuration may rebuild the stream. An invalid command is rejected before interruption. Control disables mutations while `reconfiguring` and displays the engine-confirmed state. `stopped` can mean a configured driver with no routes or a stream stopped by `engine.stop` with saved routes ready for `engine.start`. The HTTP/WebSocket server remains available while the engine process lives; closing the Electron window leaves the engine available to clients that depend on its API.

`GET /api/v1/state` also returns `bufferHealth`. It is `null` until the physical ASIO callback completes an observation window in the current running session, and becomes `null` again on stop or reconfiguration. A measured window reports `sampleRate`, `bufferFrames`, `windowDurationMs`, `callbackCount`, `delayedStartCount`, `processingOverrunCount`, `blockPeriodNs`, `worstProcessingNs`, `worstMarginNs`, and `state` (`insufficient`, `healthy`, or `warning`). Durations and margins use nanoseconds; a negative margin means the longest observed processing call exceeded one block period. The warning requires three consecutive windows with at least 1% delayed starts or processing overruns (and at least three incidents), and clears after three incident-free windows. Fewer than 100 callbacks or less than one second of observation is `insufficient`. Delayed callback arrival is a scheduling observation, not proof of an audible dropout. Virtual client transport underruns and overruns remain separate `audio.meter` counters.

## Events

Events have `{ "event": "<name>", "payload": { ... } }` and no `id`.

| Event | Payload |
| --- | --- |
| `engine.status` | Complete `state`, `physicalDriverId`, `sampleRate`, `bufferFrames`, `lastError` |
| `devices.changed` | `physicalDrivers` and `virtualDrivers` inventories matching `/api/v1/drivers` |
| `routes.changed` | Complete, confirmed `routes` list |
| `audio.meter` | `endpointId`, `peakDbfs`, `shortTermLufs` (null during its initial 3-second window), `lufsWindowSeconds`, cumulative `underruns` and `overruns` |
| `engine.error` | Structured `code`, `message`, and optional operation, driver, ASIO code |
| `audio.stereoPairs.changed` | `{ "stereoPairs": [...] }`, complete list of open and routed L/R pairs |
| `audio.correlation` | `stereoPairId`, L/R IDs, coefficient in `[-1, 1]` or `null`, and `state` (`measuring`, `no_signal`, `stopped`) |

Meter events for routed endpoints are emitted about twice per second. `peakDbfs` describes the latest audio block's peak. `shortTermLufs` is BS.1770 K-weighted short-term loudness over a rolling 3-second window; it is `null` until the window fills. `lufsWindowSeconds` reports the window duration. Virtual endpoints report cumulative counters for the corresponding client-ring direction; physical endpoint counters are zero. Events do not carry audio. The server publishes measurements outside the real-time callback.

Correlation compares synchronous samples of two client outputs in 100 ms windows, computing a mean-centered, normalized zero-lag coefficient at 10 Hz. Below `-90` dBFS RMS on either channel, `correlation` is `null` and `state` is `no_signal`. Zero means weak correlation, not silence. The coefficient is broadband coherence, not a phase angle in degrees. PCM samples remain in the native callback and never enter the API.

## Routing rules

A route connects individual channels and has `id`, `sourceEndpointId`, `destinationEndpointId`, `gainDb` from -120 to +24, and `mute`. Only these links are allowed:

1. TimoxVasio output to TimoxVasio input.
2. TimoxVasio output to physical ASIO output.
3. Physical ASIO input to TimoxVasio input.

Endpoint IDs must exist and directions must match. Incompatible rates or formats are rejected; no implicit sample-rate conversion is provided. The API reports only client-allocated channels; physical endpoints reflect capabilities discovered after applying the rate.

## Local security and client boundary

The engine does not bind external network interfaces. It accepts local WebSocket connections with no `Origin` header from native clients and from the declared VASIO application origin; other origins are rejected. Control reads inventories, routes, errors, and diagnostics only through the API. Shared-audio-memory structures remain private to engine and DLL.

## Diagnostics and lifecycle

`GET /api/v1/diagnostics?limit=N` returns log level (`info` or `debug`) and recent entries. `limit` defaults to 200 and accepts 1–500. Entries have `timestamp`, `level`, `component`, and `message`.

`PUT /api/v1/diagnostics` accepts exactly `{ "level": "info" }` or `{ "level": "debug" }` and persists it. Logs are written to `%LOCALAPPDATA%\TimoxVasio\logs\engine.log` with four archives. Audio callbacks never write logs. `engine.start` and `engine.stop` control audio without closing the API host; `engine.stop` is refused while a TimoxVasio client is attached.

## Engine clock source

`engine.clockSource` is `{ "kind": "asio" }` or `{ "kind": "software" }`. `engine.clockCapabilities` describes supported sample rates and buffer sizes, or is `null` when no clock is configured. `physicalDrivers` remains the physical ASIO inventory.

`configuration.apply.payload.clockSource` is optional: omission preserves the current source inside the transaction checked by `expectedConfigurationRevision`. Legacy persisted configurations without this field retain ASIO mode. Existing fields remain required. Software mode requires `physicalDriverId: null`, integer `sampleRate` between 8000 and 384000 and `bufferFrames` between 16 and 8192. It clocks the graph without a physical device. Routes to unselected hardware stay saved but inactive.

`engine.start`, `engine.stop` and `engine.reset` also apply to the software clock. A clock worker failure stops audio and publishes an error; recovery is explicit. `engine.status` events expose the same source and capabilities as HTTP state.

## WASAPI bridges — contract 1.1

The `/api/v1` paths and `vasio.api.v1` WebSocket subprotocol remain unchanged. State announces `apiVersion: "1.1"`. `GET /api/v1/wasapi-devices` refreshes endpoint inventory; state reads use cached inventory. Each endpoint may also publish its optional Windows `containerId`, which groups capture and render endpoints belonging to the same hardware device.

Engines that support per-process app audio copies expose `capabilities.wasapiProcessLoopback: true` in `/api/v1/state`. This optional field lets clients distinguish that feature from other WASAPI functions; if absent, the connected engine does not advertise it.

`GET /api/v1/windows-audio-applications` explicitly refreshes active, single-process Windows playback sessions. It returns the PID, process creation time, image path, display name, output endpoint names and `windowsAudioApplicationInventoryError`. If that field is non-null, enumeration failed; an empty array then does not mean there are no sessions. This inventory describes playback through the Windows shared audio mixer; it does not prove which application API was called. Timox ASIO clients are reported separately in the existing virtual-driver inventory. A process can appear in both inventories.

In the graph, a Timox ASIO client is direct membership: the application opened `TimoxVasio.dll` and sends its buffers to the driver. A `processLoopback` bridge is an app audio copy created by Timox: it adds the process's shared audio as a separate source. This copy does not connect the application to the driver, redirect its playback, or prove which audio driver the application selected. The two sources remain distinct even when they refer to the same process.

Each direct ASIO client also exposes `activity`: driver state, callbacks received since the last start, complete block exchanges, exchanged frames and age of the last callback. `available` says whether the driver's shared status could be read; `stable` says whether the counters came from one coherent snapshot. These counters prove callback or block-exchange delivery, not audible signal. In `bufferHealth`, `callbackCount: 0` with state `insufficient` means the selected physical ASIO master sent no callbacks during that window; ASIO clients cannot receive timed blocks then, even if the overall engine state is `running`.

`configuration.apply.payload.wasapiBridges` contains desired configuration: `id`, `kind` (`capture`, `render`, `endpointLoopback`, `processLoopback`), exactly one selector `endpointId` or `defaultRole` (`console`, `multimedia`, `communications`) for endpoint kinds, and optional `enabled` and `bufferDuration100ns`. `processLoopback` instead requires `processId`, decimal-string `processCreationTime` and `processImagePath`; it is capture-only and identity-checked before and after asynchronous activation. The engine keeps a handle to the validated process object and refuses to start or read a capture packet if that process has exited; a new process reusing its PID does not replace the held handle. The bridge adds an independent graph source from a copy of shared-mode audio produced by the target process **and its child processes**. Original playback continues independently. Windows capture covers their shared audio outputs and is not limited to one endpoint. Routing the copy back to the same output as the original can play it twice. ASIO and WASAPI-exclusive streams are not captured. Omitting the array preserves existing bridges; `[]` removes them. Associated routes must be explicitly removed when deleting a bridge.

State separates `configuredWasapiBridges` from `wasapiBridges`: the latter reports effective format, generation, retry and `health` counters (loss, silence, drift and errors). Graph ports use `wasapi:<id>:input:<channel>` or `wasapi:<id>:output:<channel>`, with channels starting at 1. Configure a bridge before discovering its ports.

`health.controlBacklogUpdates` counts control observations forced after 100 ms of unfinished draining. Cumulative within the current generation, it indicates an observation of a partial processing span, not lost audio. Its absence on an older API 1.1 engine means unavailable, not zero. `correctionPpm` includes the response to FIFO occupancy and does not directly measure hardware clock drift.

This stage supports shared mode, endpoint loopback and per-process loopback of shared playback. It does not provide a native Windows virtual driver or capture exclusive-mode and ASIO streams. See the [ASIO/WASAPI guide](ASIO_WASAPI_GUIDE.en.md) and JSON schema for exact fields and limits.
