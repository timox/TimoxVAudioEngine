# MIDI 1.0 patchbay

[Français](MIDI_PATCHBAY.md) | [English](MIDI_PATCHBAY.en.md)

The engine connects MIDI 1.0 ports to each other, like a hardware patchbay. It uses **Windows MIDI Services** (preview 12) but works only with **MIDI 1.0 ports**: the ports MIDI 1.0 applications (WinMM, WinRT MIDI 1.0) see. ASIO does not carry MIDI: these ports are separate from the audio driver.

## Ports

| Id | Role |
|---|---|
| `midi:virtual:<id>` | Port created by the engine: a MIDI 1.0 input and output that applications (DAWs, soft synths) open. What they send enters the patchbay; what the patchbay sends there reaches them. |
| `midi:port:<group>:<Windows id>` | One MIDI 1.0 port of an endpoint: USB, network or Bluetooth interface, or another application's port. |

Windows MIDI Services maps each MIDI 1.0 port of a device to one **group** (1 to 16) of its endpoint, so a 4-port interface gives `midi:port:1:…` to `midi:port:4:…`. For each port the API reports its name (as shown to MIDI 1.0 applications), whether it can be a source (`source`, MIDI In) and a destination (`destination`, MIDI Out). Direction comes from the device's function blocks, or from its USB group terminal blocks.

A virtual port is a Windows MIDI Services virtual device owned by the engine and declared **MIDI 1.0 only**: one bidirectional block on group 1. MIDI 1.0 applications see it as the port name followed by "MIDI", so names are limited to 24 ASCII characters. It exists while the engine runs. Its stable identity (`ProductInstanceId` = `TimoxVasio-<id>`) lets applications find it again after a restart.

The engine opens an endpoint only when an enabled route uses one of its ports; one connection carries all of the endpoint's ports. Other applications can use it at the same time: Windows MIDI Services is multi-client.

## Routes

A route links a source port to a destination port. Several routes can leave one source (split) or reach one destination (merge). Options, all optional:

- `channels`: accepted source channels (1 to 16, all by default);
- `messages`: accepted categories: `notes`, `polyPressure`, `controlChange`, `programChange`, `channelPressure`, `pitchBend`, `systemRealtime` (clock, start/stop), `systemCommon`, `sysex`;
- `outputChannel`: rewrites the channel;
- `transpose`: semitone offset. A note moved outside 0 to 127 is dropped, for note on and note off alike, so no note hangs;
- `enabled`: turns the route off without deleting it.

A route stays inactive when its source cannot send, its destination cannot receive, or a port is absent. A route from a port to itself is rejected. The engine does not detect loops that go through an application echoing its input to its output (MIDI thru).

Only MIDI 1.0 messages travel. When a MIDI 2.0 device sends MIDI 2.0 messages, the engine translates them to MIDI 1.0 with the default UMP translation: values scaled down, bank sent as CC 0/32 before the program change, RPN/NRPN sent as CC 101/100 or 99/98 then 6/38. Messages with no MIDI 1.0 form (per-note and relative controllers) are dropped, as are utility, UMP stream and flex data messages.

Routing runs on Windows MIDI Services receive threads, never in the audio callback. It does not allocate; the graph is swapped as a whole on each configuration change.

## API

- `GET /api/v1/midi` re-reads the MIDI 1.0 port list and reopens routed ports that came back (device plugged in again). It then returns ports, configuration and, per route, `active`, `forwarded` and `filtered`.
- `PUT /api/v1/midi/configuration` replaces the full configuration, applies it and saves it to `%LOCALAPPDATA%\TimoxVasio\midi-patchbay.json`.

Unchanged virtual ports stay open for applications; a renamed port is recreated. If a virtual port cannot be created, the previous configuration stays in effect. If an endpoint cannot be opened, only the routes using its ports stay inactive and the error appears in `ports[].error`.

Error codes: `INVALID_MIDI_CONFIGURATION` (400), `MIDI_CONFIGURATION_FAILED` (500), `MIDI_UNAVAILABLE` (503). The engine starts normally without Windows MIDI Services; only the MIDI patchbay is unavailable.

## Requirements and build

Runtime: Windows 11 with the Windows MIDI Services preview 12 runtime (Tools 0.99.94-preview.12, Transports 1.0.43-preview.12), with the virtual device transport available.

Preview 12 ships no new API package. Build against the `Windows.Devices.Midi2` NuGet metadata **0.99.92-preview.11**. The build downloads nothing:

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A x64 `
  -DTIMOX_ENABLE_WINDOWS_MIDI_SERVICES=ON `
  -DTIMOX_MIDI2_WINMD="C:\path\to\Windows.Devices.Midi2.winmd"
```

CMake generates the C++/WinRT projection with the Windows SDK `cppwinrt.exe`. Without the option the engine builds as before and `GET /api/v1/midi` reports `available: false`.

## Limitations

- The Windows backend (`src/windows_midi_services_backend.cpp`) has not yet been compiled or run on a Windows machine with preview 12. Automated tests cover routing and configuration handling with a simulated backend.
- The preview lists a known issue on Windows 11 build 26100.8875 where virtual endpoints can become unusable (microsoft/MIDI#1047).
- Port names and directions come from the MIDI 1.0 name table and the blocks the device declares. This mapping has not been checked with real devices yet.
- Preview 12 Basic MIDI 1.0 Loopback ports are not used: one endpoint echoes everything written to it back to its readers, so the engine would receive its own messages. Creating them also requires Developer mode in this preview.
- No GUI yet, and no MIDI clock locked to the audio clock.
- Supervision (`supervision.adherents[].capabilities.midi`) stays `null`: MIDI ports are not yet attached to audio adherents.
