#pragma once

#include <functional>
#include <string>
#include <cstdint>
#include <atomic>

/**
 * Linux audio capture using PipeWire (0.3.26+).
 *
 * Process-level audio isolation:
 * - Include mode: Captures audio from the target process's PipeWire node
 * - Exclude mode: Captures from default sink monitor
 *   (full per-process exclusion is not natively supported on Linux;
 *    a warning is emitted at runtime)
 *
 * Uses pimpl pattern to encapsulate PipeWire internals.
 */
class PipewireCapture {
public:
    struct AudioMetadata {
        uint32_t sampleRate;
        uint16_t channels;
        uint16_t bitsPerSample;
        bool isFloat;
    };
    using DataCallback = std::function<void(const uint8_t* data, size_t length, AudioMetadata metadata)>;

    PipewireCapture();
    ~PipewireCapture();

    /**
     * Initialize the capture session.
     * @param processId Target process ID (PID)
     * @param isIncludeMode true = capture only target's audio, false = capture all system audio
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

public:
    struct Impl;
    Impl* pImpl;
    std::atomic<bool> isCapturing{false};
    DataCallback onData;
};

/**
 * Get the owning process ID for a given X11 Window ID.
 * Falls back to 0 on Wayland without XWayland.
 * @param windowId X11 Window ID from Electron's desktopCapturer
 * @return Process ID, or 0 on failure
 */
uint32_t getPidFromWindowId(uint32_t windowId);
