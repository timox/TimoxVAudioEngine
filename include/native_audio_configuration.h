#pragma once

// Desired configuration only. Persisting this value never opens a transport,
// installs a driver or grants access to the production broker.
struct NativeAudioConfiguration {
    bool enabled = false;
};
