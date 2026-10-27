#pragma once
#include "native_audio_transport.h"
#include <memory>

// Builds the production socket transport from the protected installed
// discovery record and the enrolled per-user client certificate. Missing
// installation state is reported as Unavailable; invalid enrollment fails
// closed as AccessDenied. This function performs control-plane work only.
std::unique_ptr<INativeAudioTransport> CreateInstalledNativeAudioTransport();
