/**
 * Linux N-API addon — mirrors the Windows addon.cpp interface exactly.
 *
 * Exports:
 *   startCapture(processId, isIncludeMode, callback) → boolean
 *   stopCapture() → boolean
 *   getPidFromHwnd(windowId) → number
 */

#include <napi.h>
#include "pipewire_capture.h"

static PipewireCapture capture;
static Napi::ThreadSafeFunction tsfn;

Napi::Value StartCapture(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    uint32_t processId = 0;
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
    int result = capture.Initialize(processId, isIncludeMode, errorMsg);
    if (result != 0 || !errorMsg.empty()) {
        char buf[512];
        snprintf(buf, sizeof(buf), "PipeWire Init Failed: %s (code: %d)", errorMsg.c_str(), result);
        Napi::TypeError::New(env, buf).ThrowAsJavaScriptException();
        return env.Null();
    }

    tsfn = Napi::ThreadSafeFunction::New(
        env,
        info[2].As<Napi::Function>(),
        "PipeWireCaptureCallback",
        0,
        1
    );

    auto callback = [](const uint8_t* data, size_t length, PipewireCapture::AudioMetadata metadata) {
        if (!tsfn) return;

        struct Payload {
            std::vector<uint8_t> buffer;
            PipewireCapture::AudioMetadata meta;
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

Napi::Value GetPidFromHwnd(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (info.Length() < 1 || !info[0].IsNumber()) {
        Napi::TypeError::New(env, "Number expected").ThrowAsJavaScriptException();
        return env.Null();
    }
    uint32_t windowId = info[0].As<Napi::Number>().Uint32Value();
    uint32_t pid = getPidFromWindowId(windowId);
    return Napi::Number::New(env, pid);
}

Napi::Object Init(Napi::Env env, Napi::Object exports) {
    exports.Set(Napi::String::New(env, "startCapture"), Napi::Function::New(env, StartCapture));
    exports.Set(Napi::String::New(env, "stopCapture"), Napi::Function::New(env, StopCapture));
    exports.Set(Napi::String::New(env, "getPidFromHwnd"), Napi::Function::New(env, GetPidFromHwnd));
    return exports;
}

NODE_API_MODULE(electron_native_screenshare, Init)
