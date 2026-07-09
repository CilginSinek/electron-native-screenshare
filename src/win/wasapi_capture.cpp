#include "wasapi_capture.h"
#include <iostream>
#include <sstream>

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

// --- HRESULT helpers -------------------------------------------------------

/**
 * Returns a human-readable description for well-known WASAPI / COM HRESULTs.
 * Falls back to a hex string for unknown codes so the caller always gets
 * something actionable in the error message.
 */
static std::string HrToString(HRESULT hr) {
    char buf[64];
    switch (hr) {
        // Generic COM
        case S_OK:                                return "S_OK (success)";
        case E_FAIL:                              return "E_FAIL (0x80004005) - unspecified failure";
        case E_INVALIDARG:                        return "E_INVALIDARG (0x80070057) - one or more arguments are invalid";
        case E_OUTOFMEMORY:                       return "E_OUTOFMEMORY (0x8007000E) - out of memory";
        case E_NOINTERFACE:                       return "E_NOINTERFACE (0x80004002) - no such interface";
        case E_POINTER:                           return "E_POINTER (0x80004003) - invalid pointer";
        case E_UNEXPECTED:                        return "E_UNEXPECTED (0x8000FFFF) - catastrophic / unexpected failure. "
                                                         "Possible causes: (1) no active audio render endpoint on this system, "
                                                         "(2) Windows Audio Service is not running, "
                                                         "(3) a previous IAudioClient was not released before re-init, "
                                                         "(4) target processId is 0 or invalid when isIncludeMode=true";
        case E_ILLEGAL_METHOD_CALL:               return "E_ILLEGAL_METHOD_CALL (0x8000000E) - ActivateAudioInterfaceAsync must be "
                                                         "called from an MTA thread, not an STA thread";

        // AUDCLNT errors
        case AUDCLNT_E_NOT_INITIALIZED:           return "AUDCLNT_E_NOT_INITIALIZED (0x88890001) - IAudioClient not initialized";
        case AUDCLNT_E_ALREADY_INITIALIZED:       return "AUDCLNT_E_ALREADY_INITIALIZED (0x88890002) - IAudioClient::Initialize "
                                                         "called more than once on the same object";
        case AUDCLNT_E_WRONG_ENDPOINT_TYPE:       return "AUDCLNT_E_WRONG_ENDPOINT_TYPE (0x88890003) - render vs capture mismatch";
        case AUDCLNT_E_DEVICE_INVALIDATED:        return "AUDCLNT_E_DEVICE_INVALIDATED (0x88890004) - audio device was removed or "
                                                         "format changed while in use";
        case AUDCLNT_E_NOT_STOPPED:               return "AUDCLNT_E_NOT_STOPPED (0x88890005) - stream must be stopped first";
        case AUDCLNT_E_BAD_BUFFER_SIZE:           return "AUDCLNT_E_BAD_BUFFER_SIZE (0x88890006) - buffer size out of range";
        case AUDCLNT_E_OUT_OF_ORDER:              return "AUDCLNT_E_OUT_OF_ORDER (0x88890007) - previous GetBuffer not released";
        case AUDCLNT_E_UNSUPPORTED_FORMAT:        return "AUDCLNT_E_UNSUPPORTED_FORMAT (0x88890008) - requested WAVEFORMATEX is not "
                                                         "supported. Try letting Windows choose the format via GetMixFormat()";
        case AUDCLNT_E_INVALID_SIZE:              return "AUDCLNT_E_INVALID_SIZE (0x88890009) - invalid frame size";
        case AUDCLNT_E_DEVICE_IN_USE:             return "AUDCLNT_E_DEVICE_IN_USE (0x8889000A) - device already in exclusive use by "
                                                         "another application (e.g. Nahimic, Sonic Studio, Realtek Audio Console)";
        case AUDCLNT_E_BUFFER_OPERATION_PENDING:  return "AUDCLNT_E_BUFFER_OPERATION_PENDING (0x8889000B)";
        case AUDCLNT_E_THREAD_NOT_REGISTERED:     return "AUDCLNT_E_THREAD_NOT_REGISTERED (0x8889000C)";
        case AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED:return "AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED (0x8889000E) - exclusive mode "
                                                         "disabled in system sound settings";
        case AUDCLNT_E_ENDPOINT_CREATE_FAILED:    return "AUDCLNT_E_ENDPOINT_CREATE_FAILED (0x8889000F) - could not create audio "
                                                         "endpoint. Windows Audio Service may not be running";
        case AUDCLNT_E_SERVICE_NOT_RUNNING:       return "AUDCLNT_E_SERVICE_NOT_RUNNING (0x88890010) - Windows Audio Service (AudioSrv) "
                                                         "is stopped. Start it via services.msc or 'net start audiosrv'";
        case AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED:  return "AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED (0x88890011)";
        case AUDCLNT_E_EXCLUSIVE_MODE_ONLY:       return "AUDCLNT_E_EXCLUSIVE_MODE_ONLY (0x88890012)";
        case AUDCLNT_E_BUFDURATION_PERIOD_NOT_EQUAL:return "AUDCLNT_E_BUFDURATION_PERIOD_NOT_EQUAL (0x88890013)";
        case AUDCLNT_E_EVENTHANDLE_NOT_SET:       return "AUDCLNT_E_EVENTHANDLE_NOT_SET (0x88890014) - SetEventHandle must be called "
                                                         "before Start() when AUDCLNT_STREAMFLAGS_EVENTCALLBACK is used";
        case AUDCLNT_E_INCORRECT_BUFFER_SIZE:     return "AUDCLNT_E_INCORRECT_BUFFER_SIZE (0x88890015)";
        case AUDCLNT_E_BUFFER_SIZE_ERROR:         return "AUDCLNT_E_BUFFER_SIZE_ERROR (0x88890016)";
        case AUDCLNT_E_CPUUSAGE_EXCEEDED:         return "AUDCLNT_E_CPUUSAGE_EXCEEDED (0x88890017)";
        case AUDCLNT_E_BUFFER_ERROR:              return "AUDCLNT_E_BUFFER_ERROR (0x88890018) - GetBuffer failed";
        case AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED:   return "AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED (0x88890019)";
        case AUDCLNT_E_INVALID_DEVICE_PERIOD:     return "AUDCLNT_E_INVALID_DEVICE_PERIOD (0x88890020)";
        case AUDCLNT_E_INVALID_STREAM_FLAG:       return "AUDCLNT_E_INVALID_STREAM_FLAG (0x88890021) - invalid combination of stream "
                                                         "flags. Note: AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY cannot be used with "
                                                         "process loopback streams";
        case AUDCLNT_E_ENDPOINT_OFFLOAD_NOT_CAPABLE: return "AUDCLNT_E_ENDPOINT_OFFLOAD_NOT_CAPABLE (0x88890022)";
        case AUDCLNT_E_OUT_OF_OFFLOAD_RESOURCES:  return "AUDCLNT_E_OUT_OF_OFFLOAD_RESOURCES (0x88890023)";
        case AUDCLNT_E_OFFLOAD_MODE_ONLY:         return "AUDCLNT_E_OFFLOAD_MODE_ONLY (0x88890024)";
        case AUDCLNT_E_NONOFFLOAD_MODE_ONLY:      return "AUDCLNT_E_NONOFFLOAD_MODE_ONLY (0x88890025)";
        case AUDCLNT_E_RESOURCES_INVALIDATED:     return "AUDCLNT_E_RESOURCES_INVALIDATED (0x88890026)";

        // Windows Audio Service / system HRESULTs
        case HRESULT(0x80070005):                 return "E_ACCESSDENIED (0x80070005) - access denied. "
                                                         "The calling process may lack permission to capture audio from the target process. "
                                                         "Check that the app is not sandboxed without audio capability";
        case HRESULT(0x80070006):                 return "E_HANDLE (0x80070006) - invalid handle";
        case HRESULT(0x80070057):                 return "E_INVALIDARG (0x80070057) - invalid argument passed to WASAPI";
        case HRESULT(0x80040154):                 return "REGDB_E_CLASSNOTREG (0x80040154) - COM class not registered. "
                                                         "ActivateAudioInterfaceAsync requires Windows 10 build 2004 (20H1) or later";
        case HRESULT(0x80070490):                 return "ERROR_NOT_FOUND (0x80070490) - audio device not found";

        default:
            snprintf(buf, sizeof(buf), "0x%08lX (unknown HRESULT)", (unsigned long)hr);
            return buf;
    }
}

/**
 * Formats a composite error string that includes the failing step, HRESULT
 * code in hex, and a human-readable description.
 */
static std::string MakeError(const char* step, HRESULT hr, const char* extra = nullptr) {
    std::ostringstream ss;
    ss << step << " -> " << HrToString(hr);
    if (extra && extra[0]) ss << " | " << extra;
    return ss.str();
}

// --- Completion handler ----------------------------------------------------

// Define IAgileObject IID manually in case it's missing in some SDKs
static const IID IID_IAgileObject_Manual = { 0x94ea2b94, 0xe9cc, 0x49e0, { 0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90 } };

class AudioInterfaceCompletionHandler : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
    LONG m_cRef;
    HANDLE m_hEvent;
    IAudioClient** m_ppAudioClient;
    IUnknown* m_pUnkFTM;
    // Propagates the async activation HRESULT back to Initialize().
    HRESULT* m_pActivateHr;
public:
    AudioInterfaceCompletionHandler(HANDLE hEvent, IAudioClient** ppAudioClient, HRESULT* pActivateHr)
        : m_cRef(1), m_hEvent(hEvent), m_ppAudioClient(ppAudioClient),
          m_pUnkFTM(nullptr), m_pActivateHr(pActivateHr) {
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

        if (FAILED(hr)) {
            // GetActivateResult itself failed — very unusual
            printf("[NativeCapture] ActivateCompleted: GetActivateResult failed: %s\n",
                   HrToString(hr).c_str());
            *m_pActivateHr = hr;
        } else if (FAILED(hrActivateResult)) {
            // The async activation was rejected by Windows
            printf("[NativeCapture] ActivateCompleted: activation rejected: %s\n",
                   HrToString(hrActivateResult).c_str());
            *m_pActivateHr = hrActivateResult;
        } else if (punkAudioInterface == NULL) {
            printf("[NativeCapture] ActivateCompleted: activation succeeded but IAudioClient is NULL\n");
            *m_pActivateHr = E_POINTER;
        } else {
            hr = punkAudioInterface->QueryInterface(__uuidof(IAudioClient), (void**)m_ppAudioClient);
            punkAudioInterface->Release();
            if (FAILED(hr)) {
                printf("[NativeCapture] ActivateCompleted: QueryInterface(IAudioClient) failed: %s\n",
                       HrToString(hr).c_str());
                *m_pActivateHr = hr;
            }
            // else: *m_pActivateHr stays S_OK
        }

        SetEvent(m_hEvent);
        return S_OK;
    }
};

// --- WasapiCapture::Initialize ---------------------------------------------

#include <thread>

HRESULT WasapiCapture::Initialize(DWORD processId, bool isIncludeMode, std::string& outError) {
    // Release any leftover COM interfaces from a previous session.
    // Without this, calling Initialize() a second time leaves stale COM objects
    // that cause IAudioClient::Initialize to return 0x8000FFFF (E_UNEXPECTED).
    if (pCaptureClient) { pCaptureClient->Release(); pCaptureClient = nullptr; }
    if (pAudioClient)   { pAudioClient->Release();   pAudioClient   = nullptr; }

    HRESULT finalHr = S_OK;
    // Shared with the completion handler so async errors propagate correctly.
    HRESULT activateHr = S_OK;

    // Electron's main thread is an STA. ActivateAudioInterfaceAsync strictly requires an MTA thread.
    // If called on an STA, it throws E_ILLEGAL_METHOD_CALL. We bypass this by spawning an MTA worker.
    std::thread initThread([&]() {
        HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
        if (FAILED(hr) && hr != S_FALSE /* already MTA */) {
            outError = MakeError("CoInitializeEx(COINIT_MULTITHREADED)", hr);
            finalHr = hr;
            return;
        }

        AUDIOCLIENT_ACTIVATION_PARAMS activationParams = {};
        activationParams.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        activationParams.ProcessLoopbackParams.TargetProcessId = processId;
        activationParams.ProcessLoopbackParams.ProcessLoopbackMode =
            isIncludeMode
                ? PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE
                : PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;

        PROPVARIANT activateParams;
        PropVariantInit(&activateParams);
        activateParams.vt = VT_BLOB;
        activateParams.blob.cbSize = sizeof(activationParams);
        activateParams.blob.pBlobData = (BYTE*)&activationParams;

        HANDLE hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
        AudioInterfaceCompletionHandler* pHandler =
            new AudioInterfaceCompletionHandler(hEvent, &pAudioClient, &activateHr);

        IActivateAudioInterfaceAsyncOperation* asyncOp = nullptr;
        hr = ActivateAudioInterfaceAsync(
            VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
            __uuidof(IAudioClient),
            &activateParams,
            pHandler,
            &asyncOp);

        if (FAILED(hr)) {
            outError = MakeError("ActivateAudioInterfaceAsync (synchronous failure)", hr,
                "This is unusual. Ensure Windows 10 build 2004+ and that Windows Audio Service is running.");
            finalHr = hr;
        } else {
            WaitForSingleObject(hEvent, INFINITE);
            if (asyncOp) asyncOp->Release();

            // Propagate any async activation error
            if (FAILED(activateHr)) {
                outError = MakeError("ActivateAudioInterfaceAsync (async activation callback)", activateHr,
                    processId == 0 && isIncludeMode
                        ? "processId is 0 with isIncludeMode=true — this is invalid; provide a real target PID"
                        : nullptr);
                finalHr = activateHr;
            }
        }

        CloseHandle(hEvent);
        pHandler->Release();

        if (SUCCEEDED(finalHr)) {
            if (!pAudioClient) {
                outError = "IAudioClient is NULL after async activation completed without error "
                           "— this is an internal state inconsistency, please file a bug";
                finalHr = E_POINTER;
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

                static const GUID SUBTYPE_IEEE_FLOAT_GUID = {
                    0x00000003, 0x0000, 0x0010,
                    { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 }
                };
                wfx.SubFormat = SUBTYPE_IEEE_FLOAT_GUID;

                // Process loopback requires AUDCLNT_STREAMFLAGS_LOOPBACK and AUDCLNT_STREAMFLAGS_EVENTCALLBACK.
                // AUTOCONVERTPCM lets Windows resample if needed.
                // Note: AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY is INVALID here (causes 0x88890021).
                DWORD streamFlags =
                    AUDCLNT_STREAMFLAGS_LOOPBACK |
                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                    AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM;

                hr = pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                              streamFlags,
                                              0, 0, (WAVEFORMATEX*)&wfx, NULL);
                if (FAILED(hr)) {
                    outError = MakeError("IAudioClient::Initialize", hr,
                        "Format: Float32 stereo 48kHz, flags=LOOPBACK|EVENTCALLBACK|AUTOCONVERTPCM, "
                        "mode=SHARED. If UNSUPPORTED_FORMAT, try calling GetMixFormat() first.");
                    finalHr = hr;
                } else {
                    hr = pAudioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&pCaptureClient);
                    if (FAILED(hr)) {
                        outError = MakeError("IAudioClient::GetService(IAudioCaptureClient)", hr);
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

// --- Capture thread --------------------------------------------------------

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

                if (onData && pData) {
                    size_t bytesPerFrame = 8; // 2 channels * 4 bytes (float32)

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
