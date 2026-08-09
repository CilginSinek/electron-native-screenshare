#include "jack_capture.h"
#include <iostream>
#include <dlfcn.h>
#include <thread>
#include <chrono>

typedef struct _jack_client jack_client_t;
typedef void* (*jack_client_open_t)(const char*, int, int*, ...);
typedef int (*jack_client_close_t)(jack_client_t*);

static jack_client_open_t dyn_jack_client_open = nullptr;
static jack_client_close_t dyn_jack_client_close = nullptr;

static bool load_jack() {
    if (dyn_jack_client_open) return true;
    void* handle = dlopen("libjack.so.0", RTLD_LAZY);
    if (!handle) handle = dlopen("libjack.so", RTLD_LAZY);
    if (!handle) return false;

    dyn_jack_client_open = (jack_client_open_t)dlsym(handle, "jack_client_open");
    dyn_jack_client_close = (jack_client_close_t)dlsym(handle, "jack_client_close");
    
    if (!dyn_jack_client_open || !dyn_jack_client_close) {
        dlclose(handle);
        return false;
    }
    return true;
}

struct JackCapture::Impl {
    std::vector<uint32_t> targetPids;
    bool includeMode = false;
    std::thread captureThread;
    jack_client_t* client = nullptr;
};

JackCapture::JackCapture() : pImpl(new Impl()) {}

JackCapture::~JackCapture() {
    Stop();
    delete pImpl;
}

int JackCapture::Initialize(const std::vector<uint32_t>& processIds, bool isIncludeMode, std::string& outError) {
    if (!load_jack()) {
        outError = "JACK shared library (libjack.so.0) not found.";
        return -1;
    }

    pImpl->targetPids = processIds;
    pImpl->includeMode = isIncludeMode;
    
    int status = 0;
    pImpl->client = (jack_client_t*)dyn_jack_client_open("electron-native-screenshare", 0, &status);
    if (!pImpl->client) {
        outError = "Failed to open JACK client. Is JACK daemon running?";
        return -2;
    }

    return 0;
}

void JackCapture::Start(DataCallback callback) {
    if (isCapturing.load()) return;
    
    onData = callback;
    isCapturing.store(true);
    
    pImpl->captureThread = std::thread([this]() {
        AudioMetadata meta = {48000, 2, 32, true};
        std::vector<uint8_t> dummyBuffer(4096, 0);
        
        while (isCapturing.load()) {
            if (onData) {
                onData(dummyBuffer.data(), dummyBuffer.size(), meta);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
}

void JackCapture::Stop() {
    isCapturing.store(false);
    
    if (pImpl->captureThread.joinable()) {
        pImpl->captureThread.join();
    }
    
    if (pImpl->client) {
        dyn_jack_client_close(pImpl->client);
        pImpl->client = nullptr;
    }
}
