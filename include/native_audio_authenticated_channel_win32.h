#pragma once
#include "native_audio_broker_transport.h"
#include <Windows.h>
#include <wincrypt.h>
#include <array>

struct NativeAudioChannelCredentials {
    // Already provisioned; factory duplicates the context, never creates keys.
    PCCERT_CONTEXT clientCertificate=nullptr;
    std::array<unsigned char,32> serverCertificateSha256{};
};
// AF_UNIX local channel. Path comes from trusted host discovery/provisioning,
// never from a peer message; caller protects its directory. Credentials are
// duplicated as for the pipe factory. Uses one TLS stream through confirmation.
// Server authorizes against trusted enrollment; no claim of observing its OS
// token or validating production enrollment. No pipe/network fallback.
std::unique_ptr<INativeAudioAuthenticatedChannel> CreateWindowsNativeAudioSocketChannel(
    const NativeAudioChannelCredentials&,const char* trustedPath);
// Local fixed bootstrap endpoint. No unencrypted or direct-driver fallback.
std::unique_ptr<INativeAudioAuthenticatedChannel> CreateWindowsNativeAudioAuthenticatedChannel(
    const NativeAudioChannelCredentials&);
#if defined(TIMOX_NATIVE_CHANNEL_TEST_ENDPOINT)
// Isolated test builds only: no endpoint override in production configuration.
std::unique_ptr<INativeAudioAuthenticatedChannel> CreateWindowsNativeAudioAuthenticatedChannelForTest(
    const NativeAudioChannelCredentials&,const wchar_t* bootstrapName);
std::size_t WindowsNativeAudioChannelRetainedBytesForTest() noexcept;
std::size_t WindowsNativeAudioSocketChannelRetainedBytesForTest() noexcept;
#endif
