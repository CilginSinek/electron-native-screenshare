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
#include "pulseaudio_capture.h"
#include "jack_capture.h"
#include "alsa_capture.h"
#include <iostream>

static PulseAudioCapture paCapture;
static PipewireCapture pwCapture;
static JackCapture jackCapture;
static AlsaCapture alsaCapture;
static int activeCaptureType = 0; // 1=PA, 2=PW, 3=JACK, 4=ALSA
static Napi::ThreadSafeFunction tsfn;

Napi::Value StartCapture(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    std::vector<uint32_t> processIds;
    if (info.Length() > 0) {
        if (info[0].IsNumber()) {
            processIds.push_back(info[0].As<Napi::Number>().Uint32Value());
        } else if (info[0].IsArray()) {
            Napi::Array arr = info[0].As<Napi::Array>();
            for (uint32_t i = 0; i < arr.Length(); i++) {
                Napi::Value val = arr[i];
                if (val.IsNumber()) processIds.push_back(val.As<Napi::Number>().Uint32Value());
            }
        }
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
    int result = 0;
    
    // Fallback Chain: PulseAudio -> PipeWire -> JACK -> ALSA
    result = paCapture.Initialize(processIds, isIncludeMode, errorMsg);
    if (result == 0 && errorMsg.empty()) {
        activeCaptureType = 1;
        std::cout << "[electron-native-screenshare] Using PulseAudio Backend" << std::endl;
    } else {
        std::cout << "[electron-native-screenshare] PulseAudio failed: " << errorMsg << ". Trying PipeWire..." << std::endl;
        errorMsg.clear();
        result = pwCapture.Initialize(processIds, isIncludeMode, errorMsg);
        if (result == 0 && errorMsg.empty()) {
            activeCaptureType = 2;
            std::cout << "[electron-native-screenshare] Using PipeWire Backend" << std::endl;
        } else {
            std::cout << "[electron-native-screenshare] PipeWire failed: " << errorMsg << ". Trying JACK..." << std::endl;
            errorMsg.clear();
            result = jackCapture.Initialize(processIds, isIncludeMode, errorMsg);
            if (result == 0 && errorMsg.empty()) {
                activeCaptureType = 3;
                std::cout << "[electron-native-screenshare] Using JACK Backend" << std::endl;
            } else {
                std::cout << "[electron-native-screenshare] JACK failed: " << errorMsg << ". Trying ALSA..." << std::endl;
                errorMsg.clear();
                result = alsaCapture.Initialize(processIds, isIncludeMode, errorMsg);
                if (result == 0 && errorMsg.empty()) {
                    activeCaptureType = 4;
                    std::cout << "[electron-native-screenshare] Using ALSA Backend" << std::endl;
                } else {
                    std::cout << "[electron-native-screenshare] ALSA failed: " << errorMsg << std::endl;
                    std::cerr << "[electron-native-screenshare] All Linux audio backends failed to initialize!" << std::endl;
                    char buf[512];
                    snprintf(buf, sizeof(buf), "All Linux audio backends failed. Last error (ALSA): %s", errorMsg.c_str());
                    Napi::TypeError::New(env, buf).ThrowAsJavaScriptException();
                    return env.Null();
                }
            }
        }
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

    if (activeCaptureType == 1) {
        paCapture.Start([callback](const uint8_t* data, size_t length, PulseAudioCapture::AudioMetadata m) {
            PipewireCapture::AudioMetadata pw_meta = {m.sampleRate, m.channels, m.bitsPerSample, m.isFloat};
            callback(data, length, pw_meta);
        });
    } else if (activeCaptureType == 2) {
        pwCapture.Start(callback);
    } else if (activeCaptureType == 3) {
        jackCapture.Start([callback](const uint8_t* data, size_t length, JackCapture::AudioMetadata m) {
            PipewireCapture::AudioMetadata pw_meta = {m.sampleRate, m.channels, m.bitsPerSample, m.isFloat};
            callback(data, length, pw_meta);
        });
    } else if (activeCaptureType == 4) {
        alsaCapture.Start([callback](const uint8_t* data, size_t length, AlsaCapture::AudioMetadata m) {
            PipewireCapture::AudioMetadata pw_meta = {m.sampleRate, m.channels, m.bitsPerSample, m.isFloat};
            callback(data, length, pw_meta);
        });
    }
    return Napi::Boolean::New(env, true);
}

Napi::Value StopCapture(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (activeCaptureType == 1) paCapture.Stop();
    else if (activeCaptureType == 2) pwCapture.Stop();
    else if (activeCaptureType == 3) jackCapture.Stop();
    else if (activeCaptureType == 4) alsaCapture.Stop();
    activeCaptureType = 0;
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
