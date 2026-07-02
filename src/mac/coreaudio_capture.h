#pragma once

#include <functional>
#include <string>
#include <cstdint>
#include <atomic>

/**
 * macOS audio capture using ScreenCaptureKit (macOS 13+).
 *
 * Provides process-level audio isolation:
 * - Include mode: Captures only the target process's audio
 * - Exclude mode: Captures all system audio EXCEPT the target process
 *
 * Uses pimpl pattern to encapsulate Objective-C internals.
 */
class CoreAudioCapture {
public:
    struct AudioMetadata {
        uint32_t sampleRate;
        uint16_t channels;
        uint16_t bitsPerSample;
        bool isFloat;
    };
    using DataCallback = std::function<void(const uint8_t* data, size_t length, AudioMetadata metadata)>;

    CoreAudioCapture();
    ~CoreAudioCapture();

    /**
     * Initialize the capture session.
     * @param processId Target process ID (PID)
     * @param isIncludeMode true = capture only target's audio, false = capture all except target
     * @param outError Human-readable error message on failure
     * @return 0 on success, non-zero on failure
     */
    int Initialize(uint32_t processId, bool isIncludeMode, std::string& outError);

    /**
     * Start capturing audio. Calls callback on a background thread.
     */
    void Start(DataCallback callback);

    /**
     * Stop capturing and release resources.
     */
    void Stop();

private:
    struct Impl;
    Impl* pImpl;
    std::atomic<bool> isCapturing{false};
    DataCallback onData;
};

/**
 * Get the owning process ID for a given CGWindowID.
 * @param windowId CGWindowID from Electron's desktopCapturer
 * @return Process ID, or 0 on failure
 */
uint32_t getPidFromWindowId(uint32_t windowId);
