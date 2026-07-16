#include "pulseaudio_capture.h"
#include <iostream>
#include <cstring>
#include <thread>
#include <mutex>
#include <chrono>

#ifdef HAVE_PULSEAUDIO
#include <dlfcn.h>
#include <pulse/pulseaudio.h>

struct PulseSyms {
    pa_mainloop* (*mainloop_new)(void);
    void (*mainloop_free)(pa_mainloop*);
    pa_mainloop_api* (*mainloop_get_api)(pa_mainloop*);
    int (*mainloop_iterate)(pa_mainloop*, int, int*);
    void (*mainloop_quit)(pa_mainloop*, int);

    pa_context* (*context_new)(pa_mainloop_api*, const char*);
    void (*context_unref)(pa_context*);
    int (*context_connect)(pa_context*, const char*, pa_context_flags_t, const pa_spawn_api*);
    void (*context_disconnect)(pa_context*);
    pa_context_state_t (*context_get_state)(const pa_context*);
    void (*context_set_state_callback)(pa_context*, pa_context_notify_cb_t, void*);
    pa_operation* (*context_get_server_info)(pa_context*, pa_server_info_cb_t, void*);
    pa_operation* (*context_get_sink_input_info_list)(pa_context*, pa_sink_input_info_cb_t, void*);
    pa_operation* (*context_move_sink_input_by_name)(pa_context*, uint32_t, const char*, pa_context_success_cb_t, void*);

    pa_operation* (*context_load_module)(pa_context*, const char*, const char*, pa_context_index_cb_t, void*);
    pa_operation* (*context_unload_module)(pa_context*, uint32_t, pa_context_success_cb_t, void*);

    const char* (*proplist_gets)(const pa_proplist*, const char*);
    
    void (*operation_unref)(pa_operation*);
    pa_operation_state_t (*operation_get_state)(const pa_operation*);

    pa_stream* (*stream_new)(pa_context*, const char*, const pa_sample_spec*, const pa_channel_map*);
    void (*stream_unref)(pa_stream*);
    pa_stream_state_t (*stream_get_state)(const pa_stream*);
    void (*stream_set_state_callback)(pa_stream*, pa_stream_notify_cb_t, void*);
    void (*stream_set_read_callback)(pa_stream*, pa_stream_request_cb_t, void*);
    int (*stream_connect_record)(pa_stream*, const char*, const pa_buffer_attr*, pa_stream_flags_t);
    int (*stream_disconnect)(pa_stream*);
    int (*stream_peek)(pa_stream*, const void**, size_t*);
    void (*stream_drop)(pa_stream*);
};

static PulseSyms* pa_syms = nullptr;

static bool load_pulse() {
    if (pa_syms) return true;
    void* handle = dlopen("libpulse.so.0", RTLD_LAZY);
    if (!handle) handle = dlopen("libpulse.so", RTLD_LAZY);
    if (!handle) return false;

    PulseSyms* syms = new PulseSyms();
    syms->mainloop_new = (decltype(syms->mainloop_new))dlsym(handle, "pa_mainloop_new");
    syms->mainloop_free = (decltype(syms->mainloop_free))dlsym(handle, "pa_mainloop_free");
    syms->mainloop_get_api = (decltype(syms->mainloop_get_api))dlsym(handle, "pa_mainloop_get_api");
    syms->mainloop_iterate = (decltype(syms->mainloop_iterate))dlsym(handle, "pa_mainloop_iterate");
    syms->mainloop_quit = (decltype(syms->mainloop_quit))dlsym(handle, "pa_mainloop_quit");

    syms->context_new = (decltype(syms->context_new))dlsym(handle, "pa_context_new");
    syms->context_unref = (decltype(syms->context_unref))dlsym(handle, "pa_context_unref");
    syms->context_connect = (decltype(syms->context_connect))dlsym(handle, "pa_context_connect");
    syms->context_disconnect = (decltype(syms->context_disconnect))dlsym(handle, "pa_context_disconnect");
    syms->context_get_state = (decltype(syms->context_get_state))dlsym(handle, "pa_context_get_state");
    syms->context_set_state_callback = (decltype(syms->context_set_state_callback))dlsym(handle, "pa_context_set_state_callback");
    syms->context_get_server_info = (decltype(syms->context_get_server_info))dlsym(handle, "pa_context_get_server_info");
    syms->context_get_sink_input_info_list = (decltype(syms->context_get_sink_input_info_list))dlsym(handle, "pa_context_get_sink_input_info_list");
    syms->context_move_sink_input_by_name = (decltype(syms->context_move_sink_input_by_name))dlsym(handle, "pa_context_move_sink_input_by_name");
    syms->context_load_module = (decltype(syms->context_load_module))dlsym(handle, "pa_context_load_module");
    syms->context_unload_module = (decltype(syms->context_unload_module))dlsym(handle, "pa_context_unload_module");

    syms->proplist_gets = (decltype(syms->proplist_gets))dlsym(handle, "pa_proplist_gets");

    syms->operation_unref = (decltype(syms->operation_unref))dlsym(handle, "pa_operation_unref");
    syms->operation_get_state = (decltype(syms->operation_get_state))dlsym(handle, "pa_operation_get_state");

    syms->stream_new = (decltype(syms->stream_new))dlsym(handle, "pa_stream_new");
    syms->stream_unref = (decltype(syms->stream_unref))dlsym(handle, "pa_stream_unref");
    syms->stream_get_state = (decltype(syms->stream_get_state))dlsym(handle, "pa_stream_get_state");
    syms->stream_set_state_callback = (decltype(syms->stream_set_state_callback))dlsym(handle, "pa_stream_set_state_callback");
    syms->stream_set_read_callback = (decltype(syms->stream_set_read_callback))dlsym(handle, "pa_stream_set_read_callback");
    syms->stream_connect_record = (decltype(syms->stream_connect_record))dlsym(handle, "pa_stream_connect_record");
    syms->stream_disconnect = (decltype(syms->stream_disconnect))dlsym(handle, "pa_stream_disconnect");
    syms->stream_peek = (decltype(syms->stream_peek))dlsym(handle, "pa_stream_peek");
    syms->stream_drop = (decltype(syms->stream_drop))dlsym(handle, "pa_stream_drop");

    if (!syms->mainloop_new || !syms->context_new || !syms->stream_new) {
        delete syms;
        dlclose(handle);
        return false;
    }
    pa_syms = syms;
    return true;
}

#define my_pa_mainloop_new pa_syms->mainloop_new
#define my_pa_mainloop_free pa_syms->mainloop_free
#define my_pa_mainloop_get_api pa_syms->mainloop_get_api
#define my_pa_mainloop_iterate pa_syms->mainloop_iterate
#define my_pa_mainloop_quit pa_syms->mainloop_quit

#define my_pa_context_new pa_syms->context_new
#define my_pa_context_unref pa_syms->context_unref
#define my_pa_context_connect pa_syms->context_connect
#define my_pa_context_disconnect pa_syms->context_disconnect
#define my_pa_context_get_state pa_syms->context_get_state
#define my_pa_context_set_state_callback pa_syms->context_set_state_callback
#define my_pa_context_get_server_info pa_syms->context_get_server_info
#define my_pa_context_get_sink_input_info_list pa_syms->context_get_sink_input_info_list
#define my_pa_context_move_sink_input_by_name pa_syms->context_move_sink_input_by_name
#define my_pa_context_load_module pa_syms->context_load_module
#define my_pa_context_unload_module pa_syms->context_unload_module

#define my_pa_proplist_gets pa_syms->proplist_gets

#define my_pa_operation_unref pa_syms->operation_unref
#define my_pa_operation_get_state pa_syms->operation_get_state

#define my_pa_stream_new pa_syms->stream_new
#define my_pa_stream_unref pa_syms->stream_unref
#define my_pa_stream_get_state pa_syms->stream_get_state
#define my_pa_stream_set_state_callback pa_syms->stream_set_state_callback
#define my_pa_stream_set_read_callback pa_syms->stream_set_read_callback
#define my_pa_stream_connect_record pa_syms->stream_connect_record
#define my_pa_stream_disconnect pa_syms->stream_disconnect
#define my_pa_stream_peek pa_syms->stream_peek
#define my_pa_stream_drop pa_syms->stream_drop

struct PulseAudioCapture::Impl {
    pa_mainloop* mainloop = nullptr;
    pa_context* context = nullptr;
    pa_stream* stream = nullptr;

    bool includeMode = false;
    std::vector<uint32_t> targetPids;
    std::string defaultMonitorSource;
    std::string virtualSinkName;
    std::string virtualMonitorSource;
    std::string actualSinkName; // Real hardware sink
    
    // Loaded modules tracking to clean them up
    uint32_t nullSinkModuleId = PA_INVALID_INDEX;
    uint32_t loopbackModuleId = PA_INVALID_INDEX;

    bool contextReady = false;

    std::thread captureThread;
    std::thread monitorThread; // to poll new sink inputs periodically
    std::mutex mutex;
};

static void context_state_cb(pa_context* c, void* userdata) {
    auto* impl = static_cast<PulseAudioCapture::Impl*>(userdata);
    switch (my_pa_context_get_state(c)) {
        case PA_CONTEXT_READY:
            impl->contextReady = true;
            break;
        case PA_CONTEXT_FAILED:
        case PA_CONTEXT_TERMINATED:
            // error
            break;
        default:
            break;
    }
}

static void server_info_cb(pa_context* c, const pa_server_info* i, void* userdata) {
    auto* impl = static_cast<PulseAudioCapture::Impl*>(userdata);
    if (i && i->default_sink_name) {
        impl->actualSinkName = i->default_sink_name;
        impl->defaultMonitorSource = std::string(i->default_sink_name) + ".monitor";
    }
}

// Module loaded callbacks
static void module_null_sink_loaded_cb(pa_context* c, uint32_t idx, void* userdata) {
    auto* impl = static_cast<PulseAudioCapture::Impl*>(userdata);
    impl->nullSinkModuleId = idx;
}

static void module_loopback_loaded_cb(pa_context* c, uint32_t idx, void* userdata) {
    auto* impl = static_cast<PulseAudioCapture::Impl*>(userdata);
    impl->loopbackModuleId = idx;
}

    // Sink input scan callback
static void sink_input_cb(pa_context* c, const pa_sink_input_info* i, int eol, void* userdata) {
    if (eol != 0 || !i) return;
    auto* impl = static_cast<PulseAudioCapture::Impl*>(userdata);

    const char* pidStr = my_pa_proplist_gets(i->proplist, PA_PROP_APPLICATION_PROCESS_ID);
    if (!pidStr) return;

    uint32_t pid = (uint32_t)atoi(pidStr);

    bool isTarget = false;
    for (uint32_t targetPid : impl->targetPids) {
        if (pid == targetPid) {
            isTarget = true;
            break;
        }
    }

    if (impl->includeMode && isTarget) {
        // Move app to virtual sink so we capture it quietly
        pa_operation* opMv = my_pa_context_move_sink_input_by_name(c, i->index, impl->virtualSinkName.c_str(), nullptr, nullptr);
        if (opMv) my_pa_operation_unref(opMv);
    } else if (!impl->includeMode && isTarget) {
        // Exclude mode
        pa_operation* opMv = my_pa_context_move_sink_input_by_name(c, i->index, impl->virtualSinkName.c_str(), nullptr, nullptr);
        if (opMv) my_pa_operation_unref(opMv);
    }
}

static void run_pulse_operations_sync(PulseAudioCapture::Impl* impl, pa_operation* op) {
    if (!op) return;
    int ret = 0;
    while (my_pa_operation_get_state(op) == PA_OPERATION_RUNNING) {
        my_pa_mainloop_iterate(impl->mainloop, 1, &ret);
    }
    my_pa_operation_unref(op);
}

static void stream_read_cb(pa_stream* s, size_t length, void* userdata) {
    auto* self = static_cast<PulseAudioCapture*>(userdata);
    
    const void* data;
    if (my_pa_stream_peek(s, &data, &length) < 0) {
        return;
    }

    if (!data) {
        if (length) my_pa_stream_drop(s);
        return;
    }

    if (self->onData && length > 0) {
        PulseAudioCapture::AudioMetadata meta;
        meta.sampleRate = 48000;
        meta.channels = 2;
        meta.bitsPerSample = 32;
        meta.isFloat = true;
        self->onData(static_cast<const uint8_t*>(data), length, meta);
    }
    my_pa_stream_drop(s);
}

#else

struct PulseAudioCapture::Impl {};

#endif

PulseAudioCapture::PulseAudioCapture() : pImpl(new Impl()) {}

PulseAudioCapture::~PulseAudioCapture() {
    Stop();
    if (pImpl) {
        delete pImpl;
        pImpl = nullptr;
    }
}

int PulseAudioCapture::Initialize(const std::vector<uint32_t>& processIds, bool isIncludeMode, std::string& outError) {
#ifdef HAVE_PULSEAUDIO
    if (!load_pulse()) {
        outError = "PulseAudio shared library not found. Audio capture is unavailable.";
        return -1;
    }

    pImpl->includeMode = isIncludeMode;
    pImpl->targetPids = processIds;
    
    // Just use the first PID for naming the virtual sink if available
    uint32_t namingPid = processIds.empty() ? 0 : processIds[0];
    pImpl->virtualSinkName = "ens_virtual_sink_" + std::to_string(namingPid);
    pImpl->virtualMonitorSource = pImpl->virtualSinkName + ".monitor";

    pImpl->mainloop = my_pa_mainloop_new();
    if (!pImpl->mainloop) {
        outError = "Failed to create PulseAudio main loop";
        return -1;
    }

    pa_mainloop_api* api = my_pa_mainloop_get_api(pImpl->mainloop);
    pImpl->context = my_pa_context_new(api, "electron-screenshare-capture");
    if (!pImpl->context) {
        outError = "Failed to create PulseAudio context";
        return -2;
    }

    my_pa_context_set_state_callback(pImpl->context, context_state_cb, pImpl);
    
    if (my_pa_context_connect(pImpl->context, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
        outError = "Failed to connect to PulseAudio context";
        return -3;
    }

    int ret = 0;
    while (!pImpl->contextReady) {
        if (my_pa_mainloop_iterate(pImpl->mainloop, 1, &ret) < 0) {
            outError = "PulseAudio mainloop iterate failed";
            return -4;
        }
    }

    // Capture standard sink
    pa_operation* opInfo = my_pa_context_get_server_info(pImpl->context, server_info_cb, pImpl);
    run_pulse_operations_sync(pImpl, opInfo);

    if (pImpl->defaultMonitorSource.empty()) {
        outError = "Failed to get PulseAudio server default sink";
        return -5;
    }

    // Set up Null Sink module for Isolation
    std::string nullSinkArgs = "sink_name=" + pImpl->virtualSinkName + " sink_properties=device.description=electron_app_capture";
    pa_operation* opModNull = my_pa_context_load_module(pImpl->context, "module-null-sink", nullSinkArgs.c_str(), module_null_sink_loaded_cb, pImpl);
    run_pulse_operations_sync(pImpl, opModNull);

    // If we moved the app to a null sink, we also need loopback so they can still hear it
    // Wait, in includeMode: we record from virtualSink.monitor, but loopback to actualSink so user hears it.
    // In excludeMode: we record from actualSink.monitor, but loopback virtualSink to actualSink?
    // Exclude mode loopback won't work well because if we loop it back to actualSink, we capture it AGAIN!
    // For now we will connect loopback to hardware device directly? 
    // PulseAudio loopback is too complex for full process audio exclusion perfectly.
    // For now, simple loopback: load "module-loopback" from virtualSink.monitor to actual hardware sink.
    
    std::string loopbackArgs = "source=" + pImpl->virtualMonitorSource + " sink=" + pImpl->actualSinkName;
    if (!pImpl->includeMode) {
      // In exclude mode, we CANNOT loopback without it bleeding into our capture of actualSinkName.monitor.
      // So the user won't hear the excluded sound, or we omit loopback.
      // But in include mode we CAN loopback without bleeding, because we capture from virtualMonitorSource directly.
    }
    
    pa_operation* opModLoop = my_pa_context_load_module(pImpl->context, "module-loopback", loopbackArgs.c_str(), module_loopback_loaded_cb, pImpl);
    run_pulse_operations_sync(pImpl, opModLoop);

    return 0;
#else
    outError = "PulseAudio support was not compiled in this build.";
    return -1;
#endif
}

void PulseAudioCapture::Start(DataCallback callback) {
#ifdef HAVE_PULSEAUDIO
    if (isCapturing.load() || !pImpl->mainloop) return;

    onData = callback;
    isCapturing.store(true);

    pImpl->captureThread = std::thread([this]() {
        pa_sample_spec ss;
        ss.format = PA_SAMPLE_FLOAT32LE;
        ss.rate = 48000;
        ss.channels = 2;

        pImpl->stream = my_pa_stream_new(pImpl->context, "ScreenCapture", &ss, nullptr);
        if (!pImpl->stream) {
            std::cerr << "[electron-native-screenshare] Failed to create PulseAudio stream" << std::endl;
            isCapturing.store(false);
            return;
        }

        my_pa_stream_set_read_callback(pImpl->stream, stream_read_cb, this);

        const char* source = pImpl->includeMode ? pImpl->virtualMonitorSource.c_str() : pImpl->defaultMonitorSource.c_str();
        if (my_pa_stream_connect_record(pImpl->stream, source, nullptr, PA_STREAM_NOFLAGS) < 0) {
            std::cerr << "[electron-native-screenshare] Failed to connect PulseAudio record stream" << std::endl;
            isCapturing.store(false);
            return;
        }

        // Dedicated thread for routing to poll PA actively because PA subscription API is brittle manually
        pImpl->monitorThread = std::thread([this]() {
            while (isCapturing.load()) {
                {
                    std::lock_guard<std::mutex> lock(pImpl->mutex);
                    if (pImpl->context) {
                        pa_operation* opSink = my_pa_context_get_sink_input_info_list(pImpl->context, sink_input_cb, pImpl);
                        if (opSink) my_pa_operation_unref(opSink);
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        });

        int ret = 0;
        while (isCapturing.load()) {
            std::lock_guard<std::mutex> lock(pImpl->mutex);
            my_pa_mainloop_iterate(pImpl->mainloop, 0, &ret);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        onData = nullptr;
        if (pImpl->monitorThread.joinable()) pImpl->monitorThread.join();

        std::lock_guard<std::mutex> lock(pImpl->mutex);
        if (pImpl->stream) {
            my_pa_stream_disconnect(pImpl->stream);
            my_pa_stream_unref(pImpl->stream);
            pImpl->stream = nullptr;
        }

        if (pImpl->loopbackModuleId != PA_INVALID_INDEX && pImpl->context) {
            pa_operation* ud = my_pa_context_unload_module(pImpl->context, pImpl->loopbackModuleId, nullptr, nullptr);
            if (ud) my_pa_operation_unref(ud);
        }
        if (pImpl->nullSinkModuleId != PA_INVALID_INDEX && pImpl->context) {
            pa_operation* ud = my_pa_context_unload_module(pImpl->context, pImpl->nullSinkModuleId, nullptr, nullptr);
            if (ud) my_pa_operation_unref(ud);
        }

        if (pImpl->context) {
            my_pa_context_disconnect(pImpl->context);
            my_pa_context_unref(pImpl->context);
            pImpl->context = nullptr;
        }
        if (pImpl->mainloop) {
            my_pa_mainloop_free(pImpl->mainloop);
            pImpl->mainloop = nullptr;
        }
    });
#endif
}

void PulseAudioCapture::Stop() {
#ifdef HAVE_PULSEAUDIO
    if (!isCapturing.load()) return;
    isCapturing.store(false);

    if (pImpl->captureThread.joinable()) {
        pImpl->captureThread.join();
    }
#endif
}
