# MIDI 1.0 patchbay

[Français](MIDI_PATCHBAY.md) | [English](MIDI_PATCHBAY.en.md)

The engine connects MIDI 1.0 ports to each other, like a hardware patchbay. It uses **Windows MIDI Services** (preview 12) but stays **MIDI 1.0 only**: only MIDI 1.0 devices are listed, MIDI 2.0 endpoints are ignored, and only MIDI 1.0 messages travel. ASIO does not carry MIDI: these ports are separate from the audio driver.

## Ports

| Id | Role |
|---|---|
| `midi:virtual:<id>` | MIDI 1.0 port pair created by the engine, as with loopMIDI: applications (DAWs, soft synths) send to "<name> In" and listen to "<name> Out". |
| `midi:port:<group>:<Windows id>` | One port of a MIDI 1.0 device (USB interface or existing MIDI 1.0 driver). Native MIDI 2.0 endpoints (MIDI 2.0 devices, network MIDI 2.0, other applications' virtual ports) are not offered. |

Windows MIDI Services maps each MIDI 1.0 port of a device to one **group** (1 to 16) of its endpoint, so a 4-port interface gives `midi:port:1:…` to `midi:port:4:…`. For each port the API reports its name (as shown to MIDI 1.0 applications), whether it can be a source (`source`, MIDI In) and a destination (`destination`, MIDI Out). Direction comes from the device's function blocks, or from its USB group terminal blocks.

A virtual port is made of two Windows MIDI Services **Basic MIDI 1.0 Loopbacks**, which are true MIDI 1.0 ports. The engine never creates a MIDI 2.0 port (UMP virtual device): such ports crash some MIDI 1.0 applications.

- "<name> In": applications send to it; the engine only reads it;
- "<name> Out": the engine writes to it; applications read it.

A loopback echoes everything written to it back to its readers. With one direction per loopback, the engine never receives its own messages. Names are limited to 24 ASCII characters and ids to 24 characters, since they form the unique ids `tx-<id>-in` and `tx-<id>-out` (32 characters max). These stable ids let applications find the ports again after an engine restart. Loopbacks are transient: they disappear when the MIDI service restarts, and the engine reuses the ones it left behind after a crash. Windows feedback protection stays on: if an application sends "Out" back into "In", Windows mutes the loopback.

The engine opens an endpoint only when an enabled route uses one of its ports; one connection carries all of the endpoint's ports. Other applications can use it at the same time: Windows MIDI Services is multi-client.

## Routes

A route links a source port to a destination port. Several routes can leave one source (split) or reach one destination (merge). Options, all optional:

- `channels`: accepted source channels (1 to 16, all by default);
- `messages`: accepted categories: `notes`, `polyPressure`, `controlChange`, `programChange`, `channelPressure`, `pitchBend`, `systemRealtime` (clock, start/stop), `systemCommon`, `sysex`;
- `outputChannel`: rewrites the channel;
- `transpose`: semitone offset. A note moved outside 0 to 127 is dropped, for note on and note off alike, so no note hangs;
- `enabled`: turns the route off without deleting it.

A route stays inactive when its source cannot send, its destination cannot receive, or a port is absent. A route from a port to itself is rejected. The engine does not detect loops that go through an application echoing its input to its output (MIDI thru).

Only MIDI 1.0 messages travel. Any other message (MIDI 2.0, utility, UMP stream, flex data) is dropped; there is no MIDI 2.0 translation.

Routing runs on Windows MIDI Services receive threads, never in the audio callback. It does not allocate; the graph is swapped as a whole on each configuration change.

## API

- `GET /api/v1/midi` re-reads the MIDI 1.0 port list and reopens routed ports that came back (device plugged in again). It then returns ports, configuration and, per route, `active`, `forwarded` and `filtered`.
- `PUT /api/v1/midi/configuration` replaces the full configuration, applies it and saves it to `%LOCALAPPDATA%\TimoxVasio\midi-patchbay.json`.

Unchanged virtual ports stay open for applications; a renamed port is recreated. If a virtual port cannot be created, the previous configuration stays in effect. If an endpoint cannot be opened, only the routes using its ports stay inactive and the error appears in `ports[].error`.

Error codes: `INVALID_MIDI_CONFIGURATION` (400), `MIDI_CONFIGURATION_FAILED` (500), `MIDI_UNAVAILABLE` (503). The engine starts normally without Windows MIDI Services; only the MIDI patchbay is unavailable.

## Requirements and build

Runtime: Windows 11 with the Windows MIDI Services preview 12 runtime (Tools 0.99.94-preview.12, Transports 1.0.43-preview.12), with the Basic MIDI 1.0 Loopback transport installed and turned on (Transports page of the MIDI Troubleshooting and Repair tool). According to the release notes this preview transport is unsigned and needs Windows Developer mode.

Preview 12 ships no new API package. Build against the `Windows.Devices.Midi2` NuGet metadata **0.99.92-preview.11**. The build downloads nothing:

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A x64 `
  -DTIMOX_ENABLE_WINDOWS_MIDI_SERVICES=ON `
  -DTIMOX_MIDI2_WINMD="C:\path\to\Windows.Devices.Midi2.winmd"
```

CMake generates the C++/WinRT projection with the Windows SDK `cppwinrt.exe`. Without the option the engine builds as before and `GET /api/v1/midi` reports `available: false`.

## Limitations

- The Windows backend (`src/windows_midi_services_backend.cpp`) has not yet been compiled or run on a Windows machine with preview 12. Automated tests cover routing and configuration handling with a simulated backend.
- Port names and directions come from the MIDI 1.0 name table and the blocks the device declares. This mapping has not been checked with real devices yet.
- The loopback endpoint id format is not documented. The engine finds its leftover loopbacks by looking for `tx-<id>-in` / `tx-<id>-out` in it; to be checked on Windows.
- No GUI yet, and no MIDI clock locked to the audio clock.
- Supervision (`supervision.adherents[].capabilities.midi`) stays `null`: MIDI ports are not yet attached to audio adherents.
