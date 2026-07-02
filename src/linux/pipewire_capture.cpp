/**
 * Linux Audio Capture via PipeWire (0.3.26+)
 *
 * Process-level audio capture:
 * - Include mode: Creates a pw_stream connected to the target process's audio node
 * - Exclude mode: Captures from the default sink's monitor source
 *   NOTE: Per-process audio exclusion is an OS-level limitation on Linux.
 *         A console warning is emitted when exclude mode is used.
 *
 * Audio output: float32, stereo, 48kHz — matching Windows WASAPI and macOS ScreenCaptureKit.
 */

#include "pipewire_capture.h"
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/debug/types.h>
#include <spa/param/audio/type-info.h>
#include <iostream>
#include <thread>
#include <mutex>
#include <cstring>

// --- Pimpl internals ---

struct PipewireCapture::Impl {
    struct pw_main_loop* loop = nullptr;
    struct pw_context* context = nullptr;
    struct pw_core* core = nullptr;
    struct pw_stream* stream = nullptr;
    struct pw_registry* registry = nullptr;
    struct spa_hook registryListener = {};
    struct spa_hook streamListener = {};

    uint32_t targetPid = 0;
    bool includeMode = false;
    uint32_t targetNodeId = PW_ID_ANY;

    std::thread captureThread;
    std::mutex mutex;
    bool pipewireInitialized = false;
};

// --- PipeWire stream event handlers ---

static void onStreamProcess(void* userdata) {
    PipewireCapture* self = static_cast<PipewireCapture*>(userdata);
    // Access through a friend-like pattern via the public Start() that stores the callback
    // The actual buffer reading is done here
    struct pw_buffer* b = pw_stream_dequeue_buffer(self->pImpl->stream);
    if (!b) return;

    struct spa_buffer* buf = b->buffer;
    if (!buf->datas[0].data) {
        pw_stream_queue_buffer(self->pImpl->stream, b);
        return;
    }

    uint8_t* data = static_cast<uint8_t*>(buf->datas[0].data);
    uint32_t size = buf->datas[0].chunk->size;

    if (self->onData && size > 0) {
        PipewireCapture::AudioMetadata meta;
        meta.sampleRate = 48000;
        meta.channels = 2;
        meta.bitsPerSample = 32;
        meta.isFloat = true;
        self->onData(data, size, meta);
    }

    pw_stream_queue_buffer(self->pImpl->stream, b);
}

static void onStreamStateChanged(void* userdata, enum pw_stream_state old,
                                  enum pw_stream_state state, const char* error) {
    if (error) {
        std::cerr << "[electron-native-screenshare] PipeWire stream state: "
                  << pw_stream_state_as_string(state) << " — " << error << std::endl;
    }
}

static const struct pw_stream_events streamEvents = []() {
    struct pw_stream_events ev = {};
    ev.version = PW_VERSION_STREAM_EVENTS;
    ev.state_changed = onStreamStateChanged;
    ev.process = onStreamProcess;
    return ev;
}();

// --- Registry listener to find target node by PID ---

static void onRegistryGlobal(void* userdata, uint32_t id, uint32_t permissions,
                              const char* type, uint32_t version,
                              const struct spa_dict* props) {
    PipewireCapture::Impl* impl = static_cast<PipewireCapture::Impl*>(userdata);

    if (!props || strcmp(type, PW_TYPE_INTERFACE_Node) != 0) return;

    const char* mediaClass = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    if (!mediaClass) return;

    if (impl->includeMode) {
        // Include mode: find the node belonging to target PID
        if (strcmp(mediaClass, "Stream/Output/Audio") != 0) return;

        const char* pidStr = spa_dict_lookup(props, PW_KEY_APP_PROCESS_ID);
        if (!pidStr) return;

        uint32_t nodePid = (uint32_t)atoi(pidStr);
        if (nodePid == impl->targetPid) {
            impl->targetNodeId = id;
        }
    } else {
        // Exclude mode: find the default sink's monitor
        if (strcmp(mediaClass, "Audio/Sink") == 0) {
            // Use the first audio sink as the monitor target
            if (impl->targetNodeId == PW_ID_ANY) {
                impl->targetNodeId = id;
            }
        }
    }
}

static const struct pw_registry_events registryEvents = []() {
    struct pw_registry_events ev = {};
    ev.version = PW_VERSION_REGISTRY_EVENTS;
    ev.global = onRegistryGlobal;
    return ev;
}();

// --- PipewireCapture implementation ---

PipewireCapture::PipewireCapture() : pImpl(new Impl()) {}

PipewireCapture::~PipewireCapture() {
    Stop();
    if (pImpl) {
        delete pImpl;
        pImpl = nullptr;
    }
}

int PipewireCapture::Initialize(uint32_t processId, bool isIncludeMode, std::string& outError) {
    pImpl->targetPid = processId;
    pImpl->includeMode = isIncludeMode;

    // Emit warning for exclude mode — Linux limitation
    if (!isIncludeMode) {
        std::cerr << "[electron-native-screenshare] WARNING: On Linux, exclude mode captures all "
                  << "system audio from the default output. Per-process audio exclusion is not "
                  << "supported natively by PipeWire. The target process (PID: " << processId
                  << ") audio will still be present in the capture." << std::endl;
    }

    // Initialize PipeWire
    pw_init(nullptr, nullptr);
    pImpl->pipewireInitialized = true;

    pImpl->loop = pw_main_loop_new(nullptr);
    if (!pImpl->loop) {
        outError = "Failed to create PipeWire main loop";
        return -1;
    }

    pImpl->context = pw_context_new(pw_main_loop_get_loop(pImpl->loop), nullptr, 0);
    if (!pImpl->context) {
        outError = "Failed to create PipeWire context";
        return -2;
    }

    pImpl->core = pw_context_connect(pImpl->context, nullptr, 0);
    if (!pImpl->core) {
        outError = "Failed to connect to PipeWire daemon. Is PipeWire running?";
        return -3;
    }

    // Enumerate nodes to find target
    pImpl->registry = pw_core_get_registry(pImpl->core, PW_VERSION_REGISTRY, 0);
    if (!pImpl->registry) {
        outError = "Failed to get PipeWire registry";
        return -4;
    }

    spa_zero(pImpl->registryListener);
    pw_registry_add_listener(pImpl->registry, &pImpl->registryListener, &registryEvents, pImpl);

    // Process pending events to discover nodes (timeout: 500ms)
    // We run the loop briefly to let the registry populate
    struct pw_loop* loop = pw_main_loop_get_loop(pImpl->loop);

    // Flush and process for a short duration
    for (int i = 0; i < 50; i++) {
        int result = pw_loop_iterate(loop, 10); // 10ms per iteration
        if (result < 0) break;
        if (pImpl->targetNodeId != PW_ID_ANY) break;
    }

    if (pImpl->includeMode && pImpl->targetNodeId == PW_ID_ANY) {
        outError = "Target process audio node not found (PID: " + std::to_string(processId)
                   + "). The process may not be producing audio yet.";
        return -5;
    }

    return 0;
}

void PipewireCapture::Start(DataCallback callback) {
    if (isCapturing.load() || !pImpl->loop) return;

    onData = callback;
    isCapturing.store(true);

    pImpl->captureThread = std::thread([this]() {
        // Audio format: float32, stereo, 48kHz
        uint8_t buffer[4096];
        struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

        struct spa_audio_info_raw rawInfo = {};
        rawInfo.format = SPA_AUDIO_FORMAT_F32;
        rawInfo.rate = 48000;
        rawInfo.channels = 2;

        const struct spa_pod* params[1];
        params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &rawInfo);

        // Create the capture stream
        struct pw_properties* props = pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Capture",
            PW_KEY_MEDIA_ROLE, "Screen",
            nullptr
        );

        if (pImpl->includeMode && pImpl->targetNodeId != PW_ID_ANY) {
            // Include mode: target the specific node
            char nodeIdStr[32];
            snprintf(nodeIdStr, sizeof(nodeIdStr), "%u", pImpl->targetNodeId);
            pw_properties_set(props, PW_KEY_TARGET_OBJECT, nodeIdStr);
        } else if (!pImpl->includeMode && pImpl->targetNodeId != PW_ID_ANY) {
            // Exclude mode: connect to default sink monitor
            char nodeIdStr[32];
            snprintf(nodeIdStr, sizeof(nodeIdStr), "%u", pImpl->targetNodeId);
            pw_properties_set(props, PW_KEY_TARGET_OBJECT, nodeIdStr);
            pw_properties_set(props, PW_KEY_STREAM_CAPTURE_SINK, "true");
        }

        pImpl->stream = pw_stream_new(pImpl->core, "electron-screenshare-capture", props);
        if (!pImpl->stream) {
            std::cerr << "[electron-native-screenshare] Failed to create PipeWire stream" << std::endl;
            isCapturing.store(false);
            return;
        }

        spa_zero(pImpl->streamListener);
        pw_stream_add_listener(pImpl->stream, &pImpl->streamListener, &streamEvents, this);

        pw_stream_connect(
            pImpl->stream,
            PW_DIRECTION_INPUT,
            PW_ID_ANY,
            (enum pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
            params, 1
        );

        // Run the main loop — blocks until Stop() quits it
        pw_main_loop_run(pImpl->loop);
    });
}

void PipewireCapture::Stop() {
    if (!isCapturing.load()) return;
    isCapturing.store(false);

    if (pImpl->loop) {
        pw_main_loop_quit(pImpl->loop);
    }

    if (pImpl->captureThread.joinable()) {
        pImpl->captureThread.join();
    }

    if (pImpl->stream) {
        pw_stream_destroy(pImpl->stream);
        pImpl->stream = nullptr;
    }
    if (pImpl->registry) {
        pw_proxy_destroy((struct pw_proxy*)pImpl->registry);
        pImpl->registry = nullptr;
    }
    if (pImpl->core) {
        pw_core_disconnect(pImpl->core);
        pImpl->core = nullptr;
    }
    if (pImpl->context) {
        pw_context_destroy(pImpl->context);
        pImpl->context = nullptr;
    }
    if (pImpl->loop) {
        pw_main_loop_destroy(pImpl->loop);
        pImpl->loop = nullptr;
    }
    if (pImpl->pipewireInitialized) {
        pw_deinit();
        pImpl->pipewireInitialized = false;
    }
}

// --- getPidFromWindowId using X11 _NET_WM_PID ---

#include <X11/Xlib.h>
#include <X11/Xatom.h>

uint32_t getPidFromWindowId(uint32_t windowId) {
    Display* display = XOpenDisplay(nullptr);
    if (!display) {
        std::cerr << "[electron-native-screenshare] Cannot open X11 display. "
                  << "Window-to-PID resolution requires X11 or XWayland." << std::endl;
        return 0;
    }

    Atom pidAtom = XInternAtom(display, "_NET_WM_PID", True);
    if (pidAtom == None) {
        XCloseDisplay(display);
        return 0;
    }

    Atom actualType;
    int actualFormat;
    unsigned long nItems, bytesAfter;
    unsigned char* prop = nullptr;

    int status = XGetWindowProperty(display, (Window)windowId, pidAtom,
                                     0, 1, False, XA_CARDINAL,
                                     &actualType, &actualFormat,
                                     &nItems, &bytesAfter, &prop);

    uint32_t pid = 0;
    if (status == Success && prop && nItems > 0) {
        pid = *(uint32_t*)prop;
        XFree(prop);
    }

    XCloseDisplay(display);
    return pid;
}
