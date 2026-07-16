#pragma once

#include <functional>
#include <string>
#include <cstdint>
#include <atomic>
#include "pipewire_capture.h"

// PulseAudio fallback capture class
class PulseAudioCapture {
public:
    using AudioMetadata = PipewireCapture::AudioMetadata;
    using DataCallback = std::function<void(const uint8_t* data, size_t length, AudioMetadata metadata)>;

    PulseAudioCapture();
    ~PulseAudioCapture();

    int Initialize(const std::vector<uint32_t>& processIds, bool isIncludeMode, std::string& outError);
    void Start(DataCallback callback);
    void Stop();

public:
    struct Impl;
    Impl* pImpl;
    std::atomic<bool> isCapturing{false};
    DataCallback onData;
};
