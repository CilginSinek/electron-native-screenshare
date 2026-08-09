#include "alsa_capture.h"
#include <iostream>
#include <dlfcn.h>
#include <thread>
#include <chrono>

typedef int (*snd_pcm_open_t)(void**, const char*, int, int);
static snd_pcm_open_t dyn_snd_pcm_open = nullptr;

static bool load_alsa() {
    if (dyn_snd_pcm_open) return true;
    void* handle = dlopen("libasound.so.2", RTLD_LAZY);
    if (!handle) handle = dlopen("libasound.so", RTLD_LAZY);
    if (!handle) return false;
    
    dyn_snd_pcm_open = (snd_pcm_open_t)dlsym(handle, "snd_pcm_open");
    if (!dyn_snd_pcm_open) {
        dlclose(handle);
        return false;
    }
    return true;
}

struct AlsaCapture::Impl {
    std::vector<uint32_t> targetPids;
    bool includeMode = false;
    std::thread captureThread;
    void* pcmHandle = nullptr;
};

AlsaCapture::AlsaCapture() : pImpl(new Impl()) {}

AlsaCapture::~AlsaCapture() {
    Stop();
    delete pImpl;
}

int AlsaCapture::Initialize(const std::vector<uint32_t>& processIds, bool isIncludeMode, std::string& outError) {
    if (!load_alsa()) {
        outError = "ALSA shared library (libasound.so.2) not found.";
        return -1;
    }

    pImpl->targetPids = processIds;
    pImpl->includeMode = isIncludeMode;
    
    return 0;
}

void AlsaCapture::Start(DataCallback callback) {
    if (isCapturing.load()) return;
    
    onData = callback;
    isCapturing.store(true);
    
    pImpl->captureThread = std::thread([this]() {
        AudioMetadata meta = {44100, 2, 16, false};
        std::vector<uint8_t> dummyBuffer(4096, 0);
        
        while (isCapturing.load()) {
            if (onData) {
                onData(dummyBuffer.data(), dummyBuffer.size(), meta);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
}

void AlsaCapture::Stop() {
    if (!isCapturing.load()) return;
    isCapturing.store(false);
    
    if (pImpl->captureThread.joinable()) {
        pImpl->captureThread.join();
    }
}
