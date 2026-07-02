#include "wasapi_capture.h"
#include <iostream>

#pragma comment(lib, "Mmdevapi.lib")

WasapiCapture::WasapiCapture() {}

WasapiCapture::~WasapiCapture() {
    Stop();
    if (pCaptureClient) pCaptureClient->Release();
    if (pAudioClient) pAudioClient->Release();
    if (pDevice) pDevice->Release();
}

#include <objidl.h>

#ifndef VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK
#define VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK L"VAD\\Process_Loopback"
#endif

// Define IAgileObject IID manually in case it's missing in some SDKs
static const IID IID_IAgileObject_Manual = { 0x94ea2b94, 0xe9cc, 0x49e0, { 0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90 } };

class AudioInterfaceCompletionHandler : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
    LONG m_cRef;
    HANDLE m_hEvent;
    IAudioClient** m_ppAudioClient;
    IUnknown* m_pUnkFTM;
public:
    AudioInterfaceCompletionHandler(HANDLE hEvent, IAudioClient** ppAudioClient)
        : m_cRef(1), m_hEvent(hEvent), m_ppAudioClient(ppAudioClient), m_pUnkFTM(nullptr) {
        IUnknown* pUnkThis = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        CoCreateFreeThreadedMarshaler(pUnkThis, &m_pUnkFTM);
    }

    ~AudioInterfaceCompletionHandler() {
        if (m_pUnkFTM) {
            m_pUnkFTM->Release();
        }
    }

    ULONG STDMETHODCALLTYPE AddRef() { return InterlockedIncrement(&m_cRef); }
    ULONG STDMETHODCALLTYPE Release() {
        ULONG ulRef = InterlockedDecrement(&m_cRef);
        if (0 == ulRef) { delete this; }
        return ulRef;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvInterface) {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
            *ppvInterface = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
            AddRef();
            return S_OK;
        }
        if (riid == IID_IAgileObject_Manual || riid == __uuidof(IAgileObject)) {
            *ppvInterface = static_cast<IAgileObject*>(this);
            AddRef();
            return S_OK;
        }
        if (riid == __uuidof(IMarshal) && m_pUnkFTM != nullptr) {
            return m_pUnkFTM->QueryInterface(riid, ppvInterface);
        }
        *ppvInterface = NULL;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) {
        HRESULT hrActivateResult = S_OK;
        IUnknown* punkAudioInterface = NULL;
        HRESULT hr = operation->GetActivateResult(&hrActivateResult, &punkAudioInterface);
        if (FAILED(hr) || FAILED(hrActivateResult) || punkAudioInterface == NULL) {
            printf("[NativeCapture] ActivateCompleted FAILED! hr=0x%08lX, hrActivateResult=0x%08lX, punk=%p\n", hr, hrActivateResult, punkAudioInterface);
        }
        if (SUCCEEDED(hr) && SUCCEEDED(hrActivateResult) && punkAudioInterface != NULL) {
            punkAudioInterface->QueryInterface(__uuidof(IAudioClient), (void**)m_ppAudioClient);
            punkAudioInterface->Release();
        }
        SetEvent(m_hEvent);
        return S_OK;
    }
};

#include <thread>

HRESULT WasapiCapture::Initialize(DWORD processId, bool isIncludeMode, std::string& outError) {
    HRESULT finalHr = S_OK;

    // Electron's main thread is an STA. ActivateAudioInterfaceAsync strictly requires an MTA thread.
    // If called on an STA, it throws E_ILLEGAL_METHOD_CALL. We bypass this by spawning an MTA worker.
    std::thread initThread([&]() {
        HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
        
        AUDIOCLIENT_ACTIVATION_PARAMS activationParams = {};
        activationParams.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        activationParams.ProcessLoopbackParams.TargetProcessId = processId;
        activationParams.ProcessLoopbackParams.ProcessLoopbackMode = isIncludeMode ? PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE : PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;

        PROPVARIANT activateParams;
        PropVariantInit(&activateParams);
        activateParams.vt = VT_BLOB;
        activateParams.blob.cbSize = sizeof(activationParams);
        activateParams.blob.pBlobData = (BYTE*)&activationParams;

        HANDLE hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
        AudioInterfaceCompletionHandler* pHandler = new AudioInterfaceCompletionHandler(hEvent, &pAudioClient);
        
        IActivateAudioInterfaceAsyncOperation* asyncOp = nullptr;
        hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &activateParams, pHandler, &asyncOp);
        
        if (FAILED(hr)) {
            outError = "ActivateAudioInterfaceAsync failed synchronously";
            finalHr = hr;
        } else {
            WaitForSingleObject(hEvent, INFINITE);
            if (asyncOp) asyncOp->Release();
        }
        
        CloseHandle(hEvent);
        pHandler->Release();

        if (SUCCEEDED(finalHr)) {
            if (!pAudioClient) {
                outError = "pAudioClient is NULL after Wait";
                finalHr = E_FAIL;
            } else {
                WAVEFORMATEXTENSIBLE wfx = {};
                wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
                wfx.Format.nChannels = 2;
                wfx.Format.nSamplesPerSec = 48000;
                wfx.Format.wBitsPerSample = 32;
                wfx.Format.nBlockAlign = (wfx.Format.nChannels * wfx.Format.wBitsPerSample) / 8;
                wfx.Format.nAvgBytesPerSec = wfx.Format.nSamplesPerSec * wfx.Format.nBlockAlign;
                wfx.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
                wfx.Samples.wValidBitsPerSample = 32;
                wfx.dwChannelMask = 3; // SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT
                
                static const GUID SUBTYPE_IEEE_FLOAT_GUID = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
                wfx.SubFormat = SUBTYPE_IEEE_FLOAT_GUID;

                // Process loopback requires AUDCLNT_STREAMFLAGS_LOOPBACK and AUDCLNT_STREAMFLAGS_EVENTCALLBACK
                // We use AUTOCONVERTPCM to let Windows resample if needed
                // Note: AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY causes AUDCLNT_E_INVALID_STREAM_FLAG (0x88890021)
                DWORD streamFlags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM;

                hr = pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                              streamFlags,
                                              0, 0, (WAVEFORMATEX*)&wfx, NULL);
                if (FAILED(hr)) {
                    outError = "IAudioClient::Initialize failed";
                    finalHr = hr;
                } else {
                    hr = pAudioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&pCaptureClient);
                    if (FAILED(hr)) {
                        outError = "GetService(IAudioCaptureClient) failed";
                        finalHr = hr;
                    }
                }
            }
        }
        CoUninitialize();
    });

    initThread.join();
    return finalHr;
}

void WasapiCapture::Start(DataCallback callback) {
    if (isCapturing || !pAudioClient) return;
    onData = callback;
    isCapturing = true;
    hCaptureThread = CreateThread(NULL, 0, CaptureThreadProc, this, 0, NULL);
}

void WasapiCapture::Stop() {
    isCapturing = false;
    if (hCaptureThread) {
        WaitForSingleObject(hCaptureThread, INFINITE);
        CloseHandle(hCaptureThread);
        hCaptureThread = nullptr;
    }
}

DWORD WINAPI WasapiCapture::CaptureThreadProc(LPVOID pContext) {
    WasapiCapture* pThis = static_cast<WasapiCapture*>(pContext);
    pThis->CaptureLoop();
    return 0;
}

void WasapiCapture::CaptureLoop() {
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    HANDLE hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    pAudioClient->SetEventHandle(hEvent);
    pAudioClient->Start();

    while (isCapturing) {
        DWORD waitResult = WaitForSingleObject(hEvent, 100);
        if (waitResult == WAIT_OBJECT_0) {
            UINT32 packetLength = 0;
            pCaptureClient->GetNextPacketSize(&packetLength);
            while (packetLength != 0) {
                BYTE* pData;
                UINT32 numFramesAvailable;
                DWORD flags;
                pCaptureClient->GetBuffer(&pData, &numFramesAvailable, &flags, NULL, NULL);

                // Here we push pData to the callback
                if (onData && pData) {
                    size_t bytesPerFrame = 8; // 2 channels * 4 bytes
                    
                    AudioMetadata meta;
                    meta.sampleRate = 48000;
                    meta.channels = 2;
                    meta.bitsPerSample = 32;
                    meta.isFloat = true;
                    onData(pData, numFramesAvailable * bytesPerFrame, meta);
                }
                pCaptureClient->ReleaseBuffer(numFramesAvailable);
                pCaptureClient->GetNextPacketSize(&packetLength);
            }
        }
    }
    pAudioClient->Stop();
    CloseHandle(hEvent);
    CoUninitialize();
}
