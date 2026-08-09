#pragma once

#include <functional>
#include <string>
#include <cstdint>
#include <atomic>
#include <vector>

class JackCapture {
public:
    struct AudioMetadata {
        uint32_t sampleRate;
        uint16_t channels;
        uint16_t bitsPerSample;
        bool isFloat;
    };
    using DataCallback = std::function<void(const uint8_t* data, size_t length, AudioMetadata metadata)>;

    JackCapture();
    ~JackCapture();

    int Initialize(const std::vector<uint32_t>& processIds, bool isIncludeMode, std::string& outError);
    void Start(DataCallback callback);
    void Stop();

public:
    struct Impl;
    Impl* pImpl;
    std::atomic<bool> isCapturing{false};
    DataCallback onData;
};
