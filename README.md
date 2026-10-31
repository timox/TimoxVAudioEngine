# TimoxVAudioEngine

Windows audio engine for the Timox suite: physical ASIO hosting, software clocks,
routing, WASAPI bridges, configuration persistence and technical HTTP/WebSocket
API. The executable remains `TimoxVirtualAsioEngine.exe`.

This source extraction retains existing behavior and documents two narrowly
scoped validation fixes. It does not establish production hardware or PWAR audio
qualification. WASAPI remains an engine module provisionally.

Read [AGENTS.md](AGENTS.md) for ownership rules and [MIGRATION.md](MIGRATION.md)
for provenance, dependencies, verification evidence and limitations.

With Visual Studio 2026 and the Windows SDK installed:

```powershell
git rev-parse --show-toplevel
git status --short --branch
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release -- /m:4
ctest --test-dir build -C Release --output-on-failure
node verify-import.cjs
```

Output: `build/Release/TimoxVirtualAsioEngine.exe`, `openapi-v1.json` and
`schemas/`. A separately verified `PwarAsioClient.exe` must be placed beside the
engine for PWAR supervision; its pinned SHA-256 is in `cmake/PwarClient.cmake`.
This repository does not install the ASIO driver or bundle the control GUI.

MIDI 1.0 patchbay (Windows MIDI Services preview 12, optional build): see
[docs/MIDI_PATCHBAY.en.md](docs/MIDI_PATCHBAY.en.md).
