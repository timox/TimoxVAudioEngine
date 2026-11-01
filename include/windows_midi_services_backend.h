#pragma once

#include <memory>
#include <string>

#include "midi_backend.h"

// Creates the Windows MIDI Services backend. Returns null and explains why when
// the engine was built without TIMOX_ENABLE_WINDOWS_MIDI_SERVICES or when the
// MIDI service is not installed on this computer. The engine keeps running
// without MIDI in that case.
std::unique_ptr<MidiBackend> CreateWindowsMidiServicesBackend(std::string& unavailableReason);
