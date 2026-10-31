# MIDI patchbay

[Français](MIDI_PATCHBAY.md) | [English](MIDI_PATCHBAY.en.md)

The engine connects MIDI ports to each other, like a hardware patchbay. It uses **Windows MIDI Services** (preview 12). ASIO does not carry MIDI: applications see ordinary Windows MIDI ports, separate from the audio driver.

## Ports

| Id | Role |
|---|---|
| `midi:virtual:<id>` | Port created by the engine. Applications (DAWs, soft synths) open it as a MIDI input and output. What they send enters the patchbay; what the patchbay sends there reaches them. |
| `midi:endpoint:<Windows id>` | Windows MIDI Services endpoint: USB, network or Bluetooth interface, or another application's port. |

A virtual port is a Windows MIDI Services virtual device owned by the engine. It exists while the engine runs. Its stable identity (`ProductInstanceId` = `TimoxVasio-<id>`) lets applications find it again after a restart. MIDI 1.0 applications (WinMM, WinRT MIDI 1.0) see it as the port name followed by "MIDI", so names are limited to 24 ASCII characters.

The engine opens an endpoint only when an enabled route uses it. Other applications can use it at the same time: Windows MIDI Services is multi-client.

## Routes

A route links a source to a destination. Several routes can leave one source (split) or reach one destination (merge). Options, all optional:

- `groups`, `channels`: accepted source groups and channels (1 to 16, all by default);
- `messages`: accepted categories: `notes`, `polyPressure`, `controlChange`, `programChange`, `channelPressure`, `pitchBend`, `systemRealtime` (clock, start/stop), `systemCommon`, `sysex`, `flexData`;
- `outputGroup`, `outputChannel`: rewrite the group or channel;
- `transpose`: semitone offset. A note moved outside 0 to 127 is dropped, for note on and note off alike, so no note hangs;
- `enabled`: turns the route off without deleting it.

Utility messages (jitter-reduction timestamps) and UMP stream messages (endpoint discovery) belong to one connection and are never routed. A route from a port to itself is rejected. The engine does not detect loops that go through an application echoing its input to its output (MIDI thru).

Routing runs on Windows MIDI Services receive threads, never in the audio callback. It works on UMP packets without allocation; the graph is swapped as a whole on each configuration change.

## API

- `GET /api/v1/midi` re-reads the endpoint list and reopens routed endpoints that came back (device plugged in again). It then returns ports, configuration and, per route, `active`, `forwarded` and `filtered`.
- `PUT /api/v1/midi/configuration` replaces the full configuration, applies it and saves it to `%LOCALAPPDATA%\TimoxVasio\midi-patchbay.json`.

Unchanged virtual ports stay open for applications; a renamed port is recreated. If a virtual port cannot be created, the previous configuration stays in effect. If an endpoint cannot be opened, only its routes stay inactive and the error appears in `ports[].error`.

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
- No GUI yet, and no MIDI clock locked to the audio clock.
- Supervision (`supervision.adherents[].capabilities.midi`) stays `null`: MIDI ports are not yet attached to audio adherents.
