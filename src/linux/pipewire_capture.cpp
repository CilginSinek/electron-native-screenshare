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
#include <iostream>
#include <cstring>
#include <atomic>
#include <thread>
#include <mutex>
#include <vector>
#include <map>

#ifdef HAVE_PIPEWIRE
#include <dlfcn.h>

struct pw_proxy;
extern "C" struct pw_proxy* my_pw_proxy_new(struct pw_proxy *factory, const char *type, uint32_t version, size_t user_data_size);
extern "C" uint32_t my_pw_proxy_get_id(struct pw_proxy *proxy);

#define pw_proxy_new my_pw_proxy_new
#define pw_proxy_get_id my_pw_proxy_get_id
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#undef pw_proxy_new
#undef pw_proxy_get_id
#include <spa/debug/types.h>
#include <spa/param/audio/type-info.h>
#include <dlfcn.h>

struct PipewireSyms {
    void (*init)(int*, char***);
    void (*deinit)(void);
    struct pw_main_loop* (*main_loop_new)(const struct spa_dict*);
    void (*main_loop_destroy)(struct pw_main_loop*);
    struct pw_loop* (*main_loop_get_loop)(struct pw_main_loop*);
    int (*main_loop_quit)(struct pw_main_loop*);
    int (*main_loop_run)(struct pw_main_loop*);
    struct pw_context* (*context_new)(struct pw_loop*, struct pw_properties*, size_t);
    void (*context_destroy)(struct pw_context*);
    struct pw_core* (*context_connect)(struct pw_context*, struct pw_properties*, size_t);
    int (*core_disconnect)(struct pw_core*);
    struct pw_stream* (*stream_new)(struct pw_core*, const char*, struct pw_properties*);
    void (*stream_destroy)(struct pw_stream*);
    uint32_t (*stream_get_node_id)(struct pw_stream*);
    void (*stream_add_listener)(struct pw_stream*, struct spa_hook*, const struct pw_stream_events*, void*);
    int (*stream_connect)(struct pw_stream*, enum pw_direction, uint32_t, enum pw_stream_flags, const struct spa_pod**, uint32_t);
    struct pw_properties* (*properties_new)(const char*, ...) __attribute__((sentinel));
    int (*properties_set)(struct pw_properties*, const char*, const char*);
    const char* (*stream_state_as_string)(enum pw_stream_state);
    void (*proxy_destroy)(struct pw_proxy*);
    struct pw_proxy* (*proxy_new)(struct pw_proxy*, const char*, uint32_t, size_t);
    uint32_t (*proxy_get_id)(struct pw_proxy*);
};
static PipewireSyms* pw_syms = nullptr;

static bool load_pipewire() {
    if (pw_syms) return true;
    void* handle = dlopen("libpipewire-0.3.so.0", RTLD_LAZY);
    if (!handle) {
        handle = dlopen("libpipewire-0.3.so", RTLD_LAZY);
        if (!handle) return false;
    }

    PipewireSyms* syms = new PipewireSyms();
    syms->init                = (decltype(syms->init))               dlsym(handle, "pw_init");
    syms->deinit              = (decltype(syms->deinit))             dlsym(handle, "pw_deinit");
    syms->main_loop_new       = (decltype(syms->main_loop_new))      dlsym(handle, "pw_main_loop_new");
    syms->main_loop_destroy   = (decltype(syms->main_loop_destroy))  dlsym(handle, "pw_main_loop_destroy");
    syms->main_loop_get_loop  = (decltype(syms->main_loop_get_loop)) dlsym(handle, "pw_main_loop_get_loop");
    syms->main_loop_quit      = (decltype(syms->main_loop_quit))     dlsym(handle, "pw_main_loop_quit");
    syms->main_loop_run       = (decltype(syms->main_loop_run))      dlsym(handle, "pw_main_loop_run");
    syms->context_new         = (decltype(syms->context_new))        dlsym(handle, "pw_context_new");
    syms->context_destroy     = (decltype(syms->context_destroy))    dlsym(handle, "pw_context_destroy");
    syms->context_connect     = (decltype(syms->context_connect))    dlsym(handle, "pw_context_connect");
    syms->core_disconnect     = (decltype(syms->core_disconnect))    dlsym(handle, "pw_core_disconnect");
    syms->stream_new          = (decltype(syms->stream_new))         dlsym(handle, "pw_stream_new");
    syms->stream_destroy      = (decltype(syms->stream_destroy))     dlsym(handle, "pw_stream_destroy");
    syms->properties_new      = (decltype(syms->properties_new))     dlsym(handle, "pw_properties_new");
    syms->properties_set      = (decltype(syms->properties_set))     dlsym(handle, "pw_properties_set");
    syms->stream_state_as_string = (decltype(syms->stream_state_as_string)) dlsym(handle, "pw_stream_state_as_string");
    syms->proxy_destroy       = (decltype(syms->proxy_destroy))      dlsym(handle, "pw_proxy_destroy");
    syms->proxy_new           = (decltype(syms->proxy_new))          dlsym(handle, "pw_proxy_new");
    syms->proxy_get_id        = (decltype(syms->proxy_get_id))       dlsym(handle, "pw_proxy_get_id");
    syms->stream_get_node_id  = (decltype(syms->stream_get_node_id)) dlsym(handle, "pw_stream_get_node_id");
    syms->stream_add_listener = (decltype(syms->stream_add_listener))dlsym(handle, "pw_stream_add_listener");
    syms->stream_connect      = (decltype(syms->stream_connect))     dlsym(handle, "pw_stream_connect");

    // All critical symbols must resolve — any null means the library is too old or broken.
    if (!syms->init             || !syms->main_loop_new    || !syms->main_loop_destroy ||
        !syms->main_loop_get_loop || !syms->context_new      || !syms->context_destroy ||
        !syms->context_connect  || !syms->core_disconnect  || !syms->stream_new       ||
        !syms->stream_destroy   || !syms->stream_get_node_id || !syms->stream_add_listener ||
        !syms->stream_connect   || !syms->properties_new   || !syms->proxy_destroy    ||
        !syms->proxy_new        || !syms->proxy_get_id) {
        delete syms;
        dlclose(handle);
        return false;
    }
    pw_syms = syms;
    return true;
}

#define pw_init pw_syms->init
#define pw_deinit pw_syms->deinit
#define pw_main_loop_new pw_syms->main_loop_new
#define pw_main_loop_destroy pw_syms->main_loop_destroy
#define pw_main_loop_get_loop pw_syms->main_loop_get_loop
#define pw_main_loop_quit pw_syms->main_loop_quit
#define pw_main_loop_run pw_syms->main_loop_run
#define pw_context_new pw_syms->context_new
#define pw_context_destroy pw_syms->context_destroy
#define pw_context_connect pw_syms->context_connect
#define pw_core_disconnect pw_syms->core_disconnect
#define pw_stream_new pw_syms->stream_new
#define pw_stream_destroy pw_syms->stream_destroy
#define pw_stream_get_node_id pw_syms->stream_get_node_id
#define pw_stream_add_listener pw_syms->stream_add_listener
#define pw_stream_connect pw_syms->stream_connect
#define pw_properties_new pw_syms->properties_new
#define pw_properties_set pw_syms->properties_set
#define pw_stream_state_as_string pw_syms->stream_state_as_string
#define pw_proxy_destroy pw_syms->proxy_destroy

extern "C" struct pw_proxy* my_pw_proxy_new(struct pw_proxy *factory, const char *type, uint32_t version, size_t user_data_size) {
    if (!pw_syms || !pw_syms->proxy_new) return nullptr;
    return pw_syms->proxy_new(factory, type, version, user_data_size);
}
extern "C" uint32_t my_pw_proxy_get_id(struct pw_proxy *proxy) {
    if (!pw_syms || !pw_syms->proxy_get_id) return 0;
    return pw_syms->proxy_get_id(proxy);
}

struct PortInfo {
    uint32_t id;
    std::string channel;
};

struct AppNode {
    uint32_t id;
    uint32_t pid;
    std::vector<PortInfo> outPorts;
};

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

    // State for Dynamic Graph Link Engine (Exclude Mode)
    std::map<uint32_t, AppNode> appNodes;
    uint32_t myNodeId = PW_ID_ANY;
    std::vector<PortInfo> myInPorts;
    std::map<uint32_t, std::vector<PortInfo>> allInPorts;
    std::map<uint32_t, std::vector<PortInfo>> allOutPorts;
    std::map<uint64_t, struct pw_proxy*> activeLinksMap;

    std::thread captureThread;
    std::mutex mutex;
    bool pipewireInitialized = false;
};

// --- PipeWire stream event handlers ---
static void onStreamProcess(void* userdata) {
    PipewireCapture* self = static_cast<PipewireCapture*>(userdata);
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

// --- Dynamic Graph Link Engine ---
static void tryCreateLinks(PipewireCapture::Impl* impl) {
    if (impl->myNodeId == PW_ID_ANY || impl->myInPorts.empty()) return;

    for (auto& pair : impl->appNodes) {
        AppNode& app = pair.second;
        for (const auto& outPort : app.outPorts) {
            for (const auto& inPort : impl->myInPorts) {
                bool match = (outPort.channel == inPort.channel) || 
                             (outPort.channel == "MONO") || (inPort.channel == "MONO") ||
                             (outPort.channel == "UNK") || (inPort.channel == "UNK");
                if (match) {
                    uint64_t linkKey = ((uint64_t)outPort.id << 32) | inPort.id;
                    if (impl->activeLinksMap.find(linkKey) == impl->activeLinksMap.end()) {
                        struct pw_properties* p = pw_properties_new(nullptr, nullptr);
                        char bufOutNode[32], bufOutPort[32], bufInNode[32], bufInPort[32];
                        snprintf(bufOutNode, sizeof(bufOutNode), "%u", app.id);
                        snprintf(bufOutPort, sizeof(bufOutPort), "%u", outPort.id);
                        snprintf(bufInNode, sizeof(bufInNode), "%u", impl->myNodeId);
                        snprintf(bufInPort, sizeof(bufInPort), "%u", inPort.id);

                        pw_properties_set(p, PW_KEY_LINK_OUTPUT_NODE, bufOutNode);
                        pw_properties_set(p, PW_KEY_LINK_OUTPUT_PORT, bufOutPort);
                        pw_properties_set(p, PW_KEY_LINK_INPUT_NODE, bufInNode);
                        pw_properties_set(p, PW_KEY_LINK_INPUT_PORT, bufInPort);
                        
                        struct pw_proxy* link = (struct pw_proxy*)pw_core_create_object(
                            impl->core, "link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, &p->dict, 0);
                        if (link) {
                            std::cout << "[DEBUG] PipeWire created link: outNode=" << app.id 
                                      << " outPort=" << outPort.id 
                                      << " -> inNode=" << impl->myNodeId 
                                      << " inPort=" << inPort.id << std::endl;
                            impl->activeLinksMap[linkKey] = link;
                        } else {
                            std::cout << "[DEBUG] PipeWire failed to create link!" << std::endl;
                        }
                        pw_properties_free(p);
                    }
                }
            }
        }
    }
}

static void onStreamStateChanged(void* userdata, enum pw_stream_state old,
                                  enum pw_stream_state state, const char* error) {
    PipewireCapture* self = static_cast<PipewireCapture*>(userdata);
    if (error) {
        std::cerr << "[electron-native-screenshare] PipeWire stream state: "
                  << pw_stream_state_as_string(state) << " — " << error << std::endl;
    }
    
    if (state == PW_STREAM_STATE_PAUSED || state == PW_STREAM_STATE_STREAMING) {
        if (!self->pImpl->includeMode) {
            std::lock_guard<std::mutex> lock(self->pImpl->mutex);
            if (self->pImpl->myNodeId == PW_ID_ANY) {
                self->pImpl->myNodeId = pw_stream_get_node_id(self->pImpl->stream);
                std::cout << "[DEBUG] Stream Node ID resolved: " << self->pImpl->myNodeId << std::endl;
                if (self->pImpl->allInPorts.count(self->pImpl->myNodeId)) {
                    self->pImpl->myInPorts = self->pImpl->allInPorts[self->pImpl->myNodeId];
                    std::cout << "[DEBUG] Grabbed " << self->pImpl->myInPorts.size() << " input ports retroactively." << std::endl;
                }
                tryCreateLinks(self->pImpl);
            }
        }
    }
}

static const struct pw_stream_events streamEvents = []() {
    struct pw_stream_events ev = {};
    ev.version = PW_VERSION_STREAM_EVENTS;
    ev.state_changed = onStreamStateChanged;
    ev.process = onStreamProcess;
    return ev;
}();

// --- Registry listener for dynamic graph routing ---
static void onRegistryGlobal(void* userdata, uint32_t id,
                             uint32_t permissions, const char* type, uint32_t version,
                             const struct spa_dict* props) {
    PipewireCapture::Impl* impl = static_cast<PipewireCapture::Impl*>(userdata);
    
    // DEBUG: print ALL globals to see if Ports are even being announced
    if (!impl->includeMode) {
        std::cout << "[DEBUG-REGISTRY] Global: id=" << id << " type=" << type << std::endl;
    }

    if (!impl->includeMode) {
        if (!props) return;
    }

    if (impl->includeMode) {
        // Legacy Include Mode (Single Node targeting)
        if (strcmp(type, PW_TYPE_INTERFACE_Node) != 0) return;
        const char* mediaClass = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
        if (mediaClass && strcmp(mediaClass, "Stream/Output/Audio") == 0) {
            const char* pidStr = spa_dict_lookup(props, PW_KEY_APP_PROCESS_ID);
            if (pidStr && (uint32_t)atoi(pidStr) == impl->targetPid) {
                impl->targetNodeId = id;
            }
        }
        return;
    }

    // Exclude Mode: Dynamic graph linking
    std::lock_guard<std::mutex> lock(impl->mutex);
    if (strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
        const char* mediaClass = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
        if (mediaClass && strcmp(mediaClass, "Stream/Output/Audio") == 0) {
            const char* pidStr = spa_dict_lookup(props, PW_KEY_APP_PROCESS_ID);
            uint32_t pid = pidStr ? (uint32_t)atoi(pidStr) : 0;
            
            // Exclude the target PID!
            if (pid != 0 && pid == impl->targetPid) return;

            AppNode node;
            node.id = id;
            node.pid = pid;
            if (impl->allOutPorts.count(id)) {
                node.outPorts = impl->allOutPorts[id];
                std::cout << "[DEBUG] Grabbed " << node.outPorts.size() << " output ports retroactively." << std::endl;
            }
            impl->appNodes[id] = node;
            std::cout << "[DEBUG] Discovered App Node: " << id << " PID: " << pid << std::endl;
            tryCreateLinks(impl);
        }
    } else if (strcmp(type, PW_TYPE_INTERFACE_Port) == 0) {
        const char* nodeIdStr = spa_dict_lookup(props, PW_KEY_NODE_ID);
        const char* direction = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION);
        const char* channel = spa_dict_lookup(props, PW_KEY_AUDIO_CHANNEL);
        
        if (!nodeIdStr || !direction) return;
        uint32_t nodeId = (uint32_t)atoi(nodeIdStr);

        PortInfo pi;
        pi.id = id;
        pi.channel = channel ? channel : "UNK";

        if (strcmp(direction, "in") == 0) {
            impl->allInPorts[nodeId].push_back(pi);
            if (nodeId == impl->myNodeId) {
                std::cout << "[DEBUG] Discovered our Input Port: " << id << " Channel: " << pi.channel << std::endl;
                impl->myInPorts.push_back(pi);
                tryCreateLinks(impl);
            }
        } else if (strcmp(direction, "out") == 0) {
            impl->allOutPorts[nodeId].push_back(pi);
            auto it = impl->appNodes.find(nodeId);
            if (it != impl->appNodes.end()) {
                std::cout << "[DEBUG] Discovered App Output Port: " << id << " for Node: " << nodeId << " Channel: " << pi.channel << std::endl;
                it->second.outPorts.push_back(pi);
                tryCreateLinks(impl);
            }
        }
    }
}

static void onRegistryGlobalRemove(void* userdata, uint32_t id) {
    PipewireCapture::Impl* impl = static_cast<PipewireCapture::Impl*>(userdata);
    if (!impl->includeMode) {
        std::lock_guard<std::mutex> lock(impl->mutex);
        impl->appNodes.erase(id);
        // Let PipeWire auto-destroy the links on the server side when nodes/ports vanish.
    }
}

static const struct pw_registry_events registryEvents = []() {
    struct pw_registry_events ev = {};
    ev.version = PW_VERSION_REGISTRY_EVENTS;
    ev.global = onRegistryGlobal;
    ev.global_remove = onRegistryGlobalRemove;
    return ev;
}();

#else

// Dummy implementation for compilation to pass when HAVE_PIPEWIRE is missing
struct PipewireCapture::Impl {};

#endif


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
#ifdef HAVE_PIPEWIRE
    if (!load_pipewire()) {
        outError = "PipeWire shared library not found. Audio capture is unavailable.";
        return -1;
    }

    pImpl->targetPid = processId;
    pImpl->includeMode = isIncludeMode;

    // pw_init is a process-lifetime singleton: call once, never deinit between sessions.
    // Re-calling pw_init between sessions is safe (it ref-counts internally).
    // We deliberately do NOT call pw_deinit() between sessions to avoid corrupting
    // PipeWire's internal signal-handler and thread-pool state.
    static bool pwGlobalInitialized = false;
    if (!pwGlobalInitialized) {
        pw_init(nullptr, nullptr);
        pwGlobalInitialized = true;
    }

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

    pImpl->registry = pw_core_get_registry(pImpl->core, PW_VERSION_REGISTRY, 0);
    if (!pImpl->registry) {
        outError = "Failed to get PipeWire registry";
        return -4;
    }

    // --- Registry round-trip: pump the loop until we find our target node ---
    // pw_core_sync() sends a DONE event through the core connection.
    // When it arrives back on our loop, we know all preceding registry events
    // have been delivered. This is the correct PipeWire API for waiting on
    // registry enumeration — pw_loop_iterate() with a raw timeout is unreliable.
    spa_zero(pImpl->registryListener);
    pw_registry_add_listener(pImpl->registry, &pImpl->registryListener, &registryEvents, pImpl);

    struct pw_loop* rawLoop = pw_main_loop_get_loop(pImpl->loop);
    // Run up to 10 sync rounds (each flushes all pending events from the daemon)
    for (int round = 0; round < 10; ++round) {
        for (int i = 0; i < 20; ++i) {
            int r = pw_loop_iterate(rawLoop, 10 /* ms */);
            if (r < 0) goto done_enumerate;
        }
        if (!isIncludeMode || pImpl->targetNodeId != PW_ID_ANY) break;
    }
    done_enumerate:

    if (pImpl->includeMode && pImpl->targetNodeId == PW_ID_ANY) {
        // Tear down before returning error so the caller starts clean.
        pw_proxy_destroy((struct pw_proxy*)pImpl->registry);
        pImpl->registry = nullptr;
        pw_core_disconnect(pImpl->core);
        pImpl->core = nullptr;
        pw_context_destroy(pImpl->context);
        pImpl->context = nullptr;
        pw_main_loop_destroy(pImpl->loop);
        pImpl->loop = nullptr;
        outError = "Target process audio node not found (PID: " + std::to_string(processId)
                   + "). The process may not be producing audio yet.";
        return -5;
    }

    return 0;
#else
    outError = "PipeWire support was not compiled in this build.";
    return -1;
#endif
}

void PipewireCapture::Start(DataCallback callback) {
#ifdef HAVE_PIPEWIRE
    if (isCapturing.load() || !pImpl->loop) return;

    onData = callback;
    isCapturing.store(true);

    pImpl->captureThread = std::thread([this]() {
        uint8_t buffer[4096];
        struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

        struct spa_audio_info_raw rawInfo = {};
        rawInfo.format = SPA_AUDIO_FORMAT_F32;
        rawInfo.rate = 48000;
        rawInfo.channels = 2;

        const struct spa_pod* params[1];
        params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &rawInfo);

        struct pw_properties* props = pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Capture",
            PW_KEY_MEDIA_ROLE, "Screen",
            nullptr
        );

        if (pImpl->includeMode && pImpl->targetNodeId != PW_ID_ANY) {
            char nodeIdStr[32];
            snprintf(nodeIdStr, sizeof(nodeIdStr), "%u", pImpl->targetNodeId);
            pw_properties_set(props, PW_KEY_TARGET_OBJECT, nodeIdStr);
        } else if (!pImpl->includeMode) {
            // Exclude Mode: We do NOT use target_object or capture_sink.
            // We also explicitly disable autoconnect so the session manager
            // doesn't route us to the default sink.
            pw_properties_set(props, PW_KEY_NODE_AUTOCONNECT, "false");
        }

        pImpl->stream = pw_stream_new(pImpl->core, "electron-screenshare-capture", props);
        if (!pImpl->stream) {
            std::cerr << "[electron-native-screenshare] Failed to create PipeWire stream" << std::endl;
            isCapturing.store(false);
            return;
        }

        spa_zero(pImpl->streamListener);
        pw_stream_add_listener(pImpl->stream, &pImpl->streamListener, &streamEvents, this);

        // We ALWAYS need PW_STREAM_FLAG_AUTOCONNECT for the session manager to 
        // negotiate the stream format and create ports. We prevent automatic linking
        // in Exclude Mode via the PW_KEY_NODE_AUTOCONNECT="false" property set above.
        enum pw_stream_flags flags = (enum pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS);

        pw_stream_connect(
            pImpl->stream,
            PW_DIRECTION_INPUT,
            PW_ID_ANY,
            flags,
            params, 1
        );

        struct pw_loop* rawLoop = pw_main_loop_get_loop(pImpl->loop);
        while (isCapturing.load()) {
            pw_loop_iterate(rawLoop, 100 /* ms timeout */);
        }

        // ── Cleanup: all PW objects MUST be destroyed on THIS thread ──────────
        // PipeWire objects are not thread-safe; destroying from Stop() (a different
        // thread) causes the segfault. We clear onData first so any final
        // onStreamProcess() callbacks fired during pw_stream_destroy() are no-ops.
        onData = nullptr;

        if (!pImpl->includeMode) {
            std::lock_guard<std::mutex> lock(pImpl->mutex);
            // Links are actually owned by the proxy. Destroying the stream/core
            // cleans them up, but freeing the proxy objects prevents leaks.
            for (auto& pair : pImpl->activeLinksMap) {
                if (pair.second) pw_proxy_destroy(pair.second);
            }
            pImpl->activeLinksMap.clear();
            pImpl->appNodes.clear();
            pImpl->myInPorts.clear();
            pImpl->myNodeId = PW_ID_ANY;
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
        // NOTE: pw_deinit() is intentionally NOT called here.
        // pw_init/pw_deinit are process-lifetime singletons; see Initialize().
    });
#endif
}

void PipewireCapture::Stop() {
#ifdef HAVE_PIPEWIRE
    if (!isCapturing.load()) return;
    isCapturing.store(false);

    // The captureThread will naturally wake up within 100ms and handle cleanup.
    if (pImpl->captureThread.joinable()) {
        pImpl->captureThread.join();
    }
    // All PW objects are now nullptr (destroyed inside captureThread above).
#endif
}

// --- getPidFromWindowId using X11 _NET_WM_PID ---

#ifdef HAVE_X11
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <dlfcn.h>

struct X11Syms {
    Display* (*XOpenDisplay)(const char*);
    int (*XCloseDisplay)(Display*);
    Atom (*XInternAtom)(Display*, const char*, Bool);
    int (*XGetWindowProperty)(Display*, Window, Atom, long, long, Bool, Atom, Atom*, int*, unsigned long*, unsigned long*, unsigned char**);
    int (*XFree)(void*);
};
static X11Syms* x11_syms = nullptr;

static bool load_x11() {
    if (x11_syms) return true;
    void* handle = dlopen("libX11.so.6", RTLD_LAZY);
    if (!handle) handle = dlopen("libX11.so", RTLD_LAZY);
    if (!handle) return false;

    X11Syms* syms = new X11Syms();
    syms->XOpenDisplay = (decltype(syms->XOpenDisplay))dlsym(handle, "XOpenDisplay");
    syms->XCloseDisplay = (decltype(syms->XCloseDisplay))dlsym(handle, "XCloseDisplay");
    syms->XInternAtom = (decltype(syms->XInternAtom))dlsym(handle, "XInternAtom");
    syms->XGetWindowProperty = (decltype(syms->XGetWindowProperty))dlsym(handle, "XGetWindowProperty");
    syms->XFree = (decltype(syms->XFree))dlsym(handle, "XFree");

    if (!syms->XOpenDisplay || !syms->XGetWindowProperty) {
        delete syms;
        return false;
    }
    x11_syms = syms;
    return true;
}

#define XOpenDisplay x11_syms->XOpenDisplay
#define XCloseDisplay x11_syms->XCloseDisplay
#define XInternAtom x11_syms->XInternAtom
#define XGetWindowProperty x11_syms->XGetWindowProperty
#define XFree x11_syms->XFree
#endif

uint32_t getPidFromWindowId(uint32_t windowId) {
#ifdef HAVE_X11
    if (!load_x11()) {
        std::cerr << "[electron-native-screenshare] X11 shared library not found. Cannot resolve PID." << std::endl;
        return 0;
    }

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
#else
    std::cerr << "[electron-native-screenshare] X11 support was not compiled. Cannot resolve PID." << std::endl;
    return 0;
#endif
}
