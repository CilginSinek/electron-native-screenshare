#pragma once

#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#undef NTDDI_VERSION
#define NTDDI_VERSION 0x0A00000A

#include <windows.h>
#include <mmdeviceapi.h>
#include <Audioclient.h>
#include <audioclientactivationparams.h>
#include <functional>
#include <functional>
#include <vector>
#include <string>
#include <thread>

class WasapiCapture {
public:
    struct AudioMetadata {
        uint32_t sampleRate;
        uint16_t channels;
        uint16_t bitsPerSample;
        bool isFloat;
    };
    using DataCallback = std::function<void(const uint8_t* data, size_t length, AudioMetadata metadata)>;

    WasapiCapture();
    ~WasapiCapture();

    HRESULT Initialize(DWORD processId, bool isIncludeMode, std::string& outError);
    void Start(DataCallback callback);
    void Stop();

private:
    IMMDevice* pDevice = nullptr;
    IAudioClient* pAudioClient = nullptr;
    IAudioCaptureClient* pCaptureClient = nullptr;
    HANDLE hCaptureThread = nullptr;
    bool isCapturing = false;
    DataCallback onData;

    static DWORD WINAPI CaptureThreadProc(LPVOID pContext);
    void CaptureLoop();
};
