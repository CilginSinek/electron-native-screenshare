#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#undef NTDDI_VERSION
#define NTDDI_VERSION 0x0A00000A

#include <napi.h>
#include "wasapi_capture.h"

WasapiCapture capture;

Napi::ThreadSafeFunction tsfn;

Napi::Value StartCapture(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    
    DWORD processId = 0;
    if (info.Length() > 0 && info[0].IsNumber()) {
        processId = info[0].As<Napi::Number>().Uint32Value();
    }

    bool isIncludeMode = false;
    if (info.Length() > 1 && info[1].IsBoolean()) {
        isIncludeMode = info[1].As<Napi::Boolean>().Value();
    }

    if (info.Length() < 3 || !info[2].IsFunction()) {
        Napi::TypeError::New(env, "Callback function expected as third argument").ThrowAsJavaScriptException();
        return env.Null();
    }

    std::string errorMsg;
    HRESULT hr = capture.Initialize(processId, isIncludeMode, errorMsg);
    if (FAILED(hr)) {
        // Build a message that is immediately actionable from the JS catch block
        // without needing to inspect native stdout.
        char buf[512];
        snprintf(buf, sizeof(buf),
            "WASAPI Init Failed [pid=%lu, includeMode=%s]: %s",
            (unsigned long)processId,
            isIncludeMode ? "true" : "false",
            errorMsg.c_str());
        Napi::TypeError::New(env, buf).ThrowAsJavaScriptException();
        return env.Null();
    }

    tsfn = Napi::ThreadSafeFunction::New(
        env,
        info[2].As<Napi::Function>(),
        "WASAPICaptureCallback",
        0,
        1
    );

    auto callback = [](const uint8_t* data, size_t length, WasapiCapture::AudioMetadata metadata) {
        if (!tsfn) return;
        
        struct Payload {
            std::vector<uint8_t> buffer;
            WasapiCapture::AudioMetadata meta;
        };
        auto* payload = new Payload{ std::vector<uint8_t>(data, data + length), metadata };
        
        auto napiCallback = [](Napi::Env env, Napi::Function jsCallback, Payload* p) {
            if (!tsfn) {
                delete p;
                return;
            }
            Napi::Object metaObj = Napi::Object::New(env);
            metaObj.Set("sampleRate", p->meta.sampleRate);
            metaObj.Set("channels", p->meta.channels);
            metaObj.Set("bitsPerSample", p->meta.bitsPerSample);
            metaObj.Set("isFloat", p->meta.isFloat);

            Napi::Buffer<uint8_t> buffer = Napi::Buffer<uint8_t>::Copy(env, p->buffer.data(), p->buffer.size());
            jsCallback.Call({ buffer, metaObj });
            delete p;
        };
        
        tsfn.NonBlockingCall(payload, napiCallback);
    };

    capture.Start(callback);
    return Napi::Boolean::New(env, true);
}

Napi::Value StopCapture(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    capture.Stop();
    if (tsfn) {
        tsfn.Release();
        tsfn = nullptr;
    }
    return Napi::Boolean::New(env, true);
}

struct EnumUWPData {
    DWORD pid;
    bool found;
};

BOOL CALLBACK EnumChildProc(HWND hwnd, LPARAM lParam) {
    char className[256];
    if (GetClassNameA(hwnd, className, sizeof(className))) {
        if (strcmp(className, "Windows.UI.Core.CoreWindow") == 0) {
            EnumUWPData* data = (EnumUWPData*)lParam;
            GetWindowThreadProcessId(hwnd, &data->pid);
            data->found = true;
            return FALSE; // Stop enumeration
        }
    }
    return TRUE;
}

Napi::Value GetPidFromHwnd(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (info.Length() < 1 || !info[0].IsNumber()) {
        Napi::TypeError::New(env, "Number expected").ThrowAsJavaScriptException();
        return env.Null();
    }
    HWND hwnd = (HWND)(uintptr_t)info[0].As<Napi::Number>().Uint32Value();
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);

    // Optimization & Fix: If the window is a UWP ApplicationFrameWindow, the actual media process is a child window.
    char className[256];
    if (GetClassNameA(hwnd, className, sizeof(className))) {
        if (strcmp(className, "ApplicationFrameWindow") == 0) {
            EnumUWPData data = { pid, false };
            EnumChildWindows(hwnd, EnumChildProc, (LPARAM)&data);
            if (data.found) {
                pid = data.pid;
            }
        }
    }

    return Napi::Number::New(env, pid);
}

Napi::Object Init(Napi::Env env, Napi::Object exports) {
    exports.Set(Napi::String::New(env, "startCapture"), Napi::Function::New(env, StartCapture));
    exports.Set(Napi::String::New(env, "stopCapture"), Napi::Function::New(env, StopCapture));
    exports.Set(Napi::String::New(env, "getPidFromHwnd"), Napi::Function::New(env, GetPidFromHwnd));
    return exports;
}

NODE_API_MODULE(topluyo_capture, Init)
