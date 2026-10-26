# TimoxVAudioEngine

- Confirm `git rev-parse --show-toplevel` and `git status` before builds or installation.
- This repository owns the technical engine API, configuration persistence, routing, clocks, WASAPI module and PWAR client supervisor. Graph product identities remain a control application concern.
- Canonical output: `build/Release/TimoxVirtualAsioEngine.exe`, with `openapi-v1.json` and `schemas/` alongside it. Keep this executable name for existing discovery clients.
- `vendor/timoxvasio/` is an immutable dependency snapshot from canonical TimoxVasio. Update from that repository, with source commit and all SHA-256 pins; never independently edit transport ABI files.
- `src/driver_version.h` belongs to the same pinned snapshot; the dependency lock covers it too.
- WASAPI remains a module during this extraction; this does not settle whether a standalone library is desirable later.
- Read `MIGRATION.md` and the import manifest before changing dependencies. Update `cmake/PwarClient.cmake` only from a verified Timox-Pwar executable. Runtime hash verification must remain enabled.
- CTest runs isolated unit/contract tests. Never start the production engine, ASIO/WASAPI hardware, real PWAR client, or shared audio qualification without explicit coordination.
- Keep generated build output and user configuration out of source imports.
