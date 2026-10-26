# TimoxVAudioEngine extraction, 2026-10-26

This is a standalone source import from the TimoxVasio working tree based on
`3bcc05f5fed34ea8404e478ef34bb230a2efe39c`, including its uncommitted changes.
`import-manifest.json` records original relative paths and original/final SHA-256.
A source commit alone does not reproduce the local snapshot. Generated files use
`source: null`. The manifest itself and build evidence are excluded from its own
hash list. Run `node verify-import.cjs` to check the imported tree.

## Boundaries retained

The engine owns technical configuration, clocks, routing, physical ASIO hosting,
the API and the PWAR subprocess supervisor. WASAPI remains a module for the first
extraction; a separate reusable library remains an open design decision. No new
API ownership, graph identity, revision or persistence format is introduced.
The executable remains `TimoxVirtualAsioEngine.exe` for existing consumers.

The ASIO driver DLL, Electron GUI, installers, hardware probes, kernel driver and
experimental native broker service are excluded. Only native broker/channel
sources already linked into the engine and their recursive header dependencies
are retained. Standalone native service qualification targets are excluded.

## Dependencies and changes

- Shared transport implementation/layout and public header are immutable under
  `vendor/timoxvasio/`. `dependency-lock.json` pins the canonical TimoxVasio commit
  and exact local bytes. The associated `src/driver_version.h` is also pinned.
  Update these files only as a coordinated snapshot from TimoxVasio. Never fork
  transport ABI changes here. Current compatibility is the unchanged source
  contract; installed DLL/runtime compatibility has not been exercised.
- Minimal ASIO SDK source/header closure, its license, README and license
  agreement are included. The project GPLv3 LICENSE and dependency notices for
  nlohmann/json and cpp-httplib are preserved. No SDK samples or binaries are
  imported. Redistribution must retain the applicable SDK file notices.
- OpenAPI, schemas and API/ASIO/WASAPI documentation come from the same snapshot.
  Existing documentation may still describe the original suite packaging;
  canonical standalone commands and paths are those below.
- CMake keeps existing engine/test source definitions and removes unrelated
  driver, service and probe targets. Both API tests now link the supervisor,
  vendored read-only transport status reader and BCrypt, which their current
  imported implementation requires. Exception
  unwinding is enabled for all MSVC targets.
- The application profiles API test now correlates measurement replies by
  request ID instead of assuming three initial WebSocket events. The imported
  server already emits a fourth `pwar.client.status` event. The original test
  failed by reading that event as an acknowledgment; this is a test adaptation,
  with no production API or executable change.
- The supervisor now requires integer protocol `version == 1` before applying
  status, responses or events. Missing/unsupported versions report protocol
  error without accepting client status/start. The local oversized-line error
  carries version 1. This is a deliberate behavioral bug fix, not an unchanged
  copy; the original and modified file hashes are recorded separately.
- The existing `AudioConfigurationStoreTests` exposed a second imported defect:
  a bridge with neither endpoint ID nor default role indexed a missing const
  JSON property and aborted. A one-line presence guard now rejects that malformed
  configuration. Existing valid configuration format and behavior are unchanged.
- Client discovery requires `PwarAsioClient.exe` beside the engine. The old
  `build_pwar_reconciled_tests` fallback is removed. Runtime SHA-256 verification
  remains mandatory. `cmake/PwarClient.cmake` pins the verified Timox-Pwar client
  `29D5E3208E10AE5FEBBB7FD3914790A87EAF12D32BE6BEF923F20D7E33566FD3`.
  Updating that pin recompiles only supervisor consumers. The client binary is
  an external packaging input, not part of this source import.

## Build and test

From this repository root on Windows with Visual Studio 2026 and Windows SDK:

```powershell
git rev-parse --show-toplevel
git status --short --branch
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release -- /m:4 /verbosity:minimal
ctest --test-dir build -C Release --output-on-failure
node verify-import.cjs
```

Build outputs stay in `build/Release/`. OpenAPI and schemas are copied beside
the engine by CMake. No build path points back to the source clone. Build paths
in the local logs naturally name the temporary staging directory.

CTest contains isolated persistence, routing/hot-swap, conversion, software
clock, mutex lifecycle, fake WASAPI/native controller, supervisor parser and
loopback HTTP API contract tests. The API tests select ephemeral ports and
temporary configuration. No test starts the production engine or PWAR client.
Hardware enumerator/backend targets are compiled but not registered in CTest.
The protocol regression was observed failing before the version gate:
`Unsupported/missing version mutated accepted state` (exit 1).

## Local verification evidence

Final results are recorded in `verification-results.json`; full compiler/test
logs are local staging evidence and are not source import entries. Initial
configuration failed because sandboxed Visual Studio FileTracker returned
E_ACCESSDENIED; elevated configuration/build succeeded. The first all-target
build exposed missing BCrypt linkage for the imported API tests; the extraction
CMake fixes that linkage and the affected targets were rebuilt.

No engine launch, physical audio, ASIO DLL loading, external network, installation,
release packaging or production configuration changes were performed. Loopback
test servers are isolated test processes. PWAR audio continuity, real driver
compatibility, hardware behavior and installation/upgrade remain unqualified.

The root `MIGRATION.md`, `AGENTS.md`, source manifest, verification script,
dependency lock, PWAR build pin and CMake are extraction additions/changes.
No source-root files or sibling repositories were modified by this extraction.

Git attributes disable automatic text normalization for this snapshot. This preserves the exact imported bytes and SHA-256 across clones; verify the committed blobs as well as the working tree before updating these pins.
