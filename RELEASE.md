# Component versions after the repository split

The public TimoxVasio v1.2.7 release is the historical combined application
(API 1.0). This repository contains later audio development and the MIDI
candidate. Engine 1.3.0-alpha.1 is an unpublished prerelease, not a new driver.
The canonical version is cmake/EngineVersion.cmake; Windows resources use its
generated header. Do not change src/driver_version.h: the ASIO driver/transport
dependency remains pinned to its original 1.2.7 snapshot.

The engine owns routing, software and ASIO reference clocks, WASAPI bridges,
configuration, API 1.1 and PWAR supervision. WASAPI is still an engine module.
PWAR transport/client belongs to timox/Timox-Pwar; the control application belongs
to timox/TimoxVSSC. Native Windows audio remains experimental. Offline tests do
not establish hardware compatibility or PWAR audio continuity.

## MIDI candidate build

The GUI MIDI candidate requires this engine built with Windows MIDI Services.
The default OFF build intentionally reports MIDI unavailable. A merge alone
does not enable MIDI. On Windows with VS 2026 and the Windows SDK:

```powershell
git rev-parse --show-toplevel
git status --short --branch
cmake -S . -B build -G "Visual Studio 18 2026" -A x64 -DTIMOX_ENABLE_WINDOWS_MIDI_SERVICES=ON -DTIMOX_MIDI2_WINMD="<verified Windows.Devices.Midi2.winmd path>"
cmake --build build --config Release -- /m:4
ctest --test-dir build -C Release --output-on-failure
node verify-import.cjs
```

Use Windows MIDI Services preview 12 runtime and Windows.Devices.Midi2
0.99.92-preview.11 metadata as described in docs/MIDI_PATCHBAY.md. No implicit
SDK downloads or disabled verification. The windows-engine.yml workflow checks
the MIDI-enabled Windows build and isolated tests with a checksum-pinned SDK.
Its successful run uploads the executable, receipt and schemas as one candidate
artifact. It does not install the MIDI runtime or validate real endpoints/audio;
portable MIDI tests use a fake backend. The MIDI-enabled engine requires C++20
for standard C++/WinRT coroutines.

Every default build writes engine-build.json beside the executable, recording
engine/API versions, exact Git revision, dirty state, MIDI option and executable
SHA-256. If building only the engine target, also build the EngineBuildInfo target.
Build from a clean committed checkout for packaging. Supply this receipt
with the executable to TimoxVSSC. Reconfigure/rebuild after version or source
changes; an old binary must never be relabelled as this candidate.

PWAR is independently versioned as **0.1.0-alpha.1**, with control protocol v1.
The engine receipt also records its required PWAR version, source commit and
client SHA-256 from cmake/PwarClient.cmake. Packaging must use that exact client;
rebuilding or versioning the client requires updating this pin and rebuilding
the engine supervisor consumers. Runtime binary hash verification stays enabled.

TimoxVSSC's artifacts.lock.json is the cross-repository assembly manifest. It
records the driver requirement, engine and PWAR revisions, binary/schema hashes
and remaining qualification. It must be updated with a verified Windows build
before packaging; source readiness and binary readiness are separate.
