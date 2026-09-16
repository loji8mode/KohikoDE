#include "PipeWireClient.h"

#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/utils/dict.h>
#include <spa/utils/result.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

namespace Kohiko
{

namespace
{

constexpr float kMaxLinearVolume = 1.5f;
constexpr std::uint32_t kDefaultMetadataSubject = 0; // PW_ID_CORE - the "global" default.audio.* keys aren't tied to one object id

float ClampVolume(float v)
{
    return std::max(0.0f, std::min(kMaxLinearVolume, v));
}

// The "default" metadata object stores default.audio.sink/source as a
// tiny JSON blob, always exactly `{"name":"<node-name>"}` in practice
// (that's the only shape WirePlumber ever writes there) - a full JSON
// parser would be a heavyweight dependency for one fixed shape, so
// this just picks the "name" field's string value out by hand, the
// same "hand-roll the one format actually in use" approach IniFile
// already takes for freedesktop .desktop-style files.
std::string ExtractJsonNameField(const char* json)
{
    if (!json)
        return {};

    const char* nameKey = std::strstr(json, "\"name\"");
    if (!nameKey)
        return {};

    const char* colon = std::strchr(nameKey, ':');
    if (!colon)
        return {};

    const char* firstQuote = std::strchr(colon, '"');
    if (!firstQuote)
        return {};

    const char* secondQuote = std::strchr(firstQuote + 1, '"');
    if (!secondQuote)
        return {};

    return std::string(firstQuote + 1, secondQuote - firstQuote - 1);
}

}

struct PipeWireClient::Impl
{
    pw_loop* loop = nullptr;
    pw_context* context = nullptr;
    pw_core* core = nullptr;
    spa_hook coreListener{};

    pw_registry* registry = nullptr;
    spa_hook registryListener{};

    pw_metadata* defaultMetadata = nullptr;
    spa_hook metadataListener{};
    std::uint32_t defaultMetadataId = SPA_ID_INVALID;

    struct NodeEntry
    {
        std::uint32_t id = 0;
        pw_proxy* proxy = nullptr;
        spa_hook listener{};
        AudioNode info;
        Impl* owner = nullptr;
    };

    std::vector<std::unique_ptr<NodeEntry>> nodes;
    std::vector<AudioNode> snapshot;

    std::string defaultSinkName;
    std::string defaultSourceName;

    ChangeHandler changeHandler;

    struct MeterStream
    {
        pw_stream* stream = nullptr;
        spa_hook listener{};
        std::atomic<float> peak{0.0f};
        Impl* owner = nullptr;
    };

    MeterStream outputMeter;
    MeterStream inputMeter;

    bool available = false;

    NodeEntry* FindNode(std::uint32_t id)
    {
        for (auto& n : nodes)
            if (n->id == id)
                return n.get();

        return nullptr;
    }

    void RebuildSnapshot()
    {
        snapshot.clear();
        snapshot.reserve(nodes.size());

        for (auto& n : nodes)
        {
            AudioNode node = n->info;
            node.isDefault = node.isSource
                ? (!defaultSourceName.empty() && node.name == defaultSourceName)
                : (!defaultSinkName.empty() && node.name == defaultSinkName);
            snapshot.push_back(std::move(node));
        }

        std::sort(snapshot.begin(), snapshot.end(), [](const AudioNode& a, const AudioNode& b)
        {
            if (a.isSource != b.isSource)
                return !a.isSource; // outputs first, matching the spec's "Output devices" / "Input devices" ordering
            return a.description < b.description;
        });
    }

    void NotifyChange()
    {
        RebuildSnapshot();
        if (changeHandler)
            changeHandler();
    }
};

namespace
{

// --- node event callbacks ---------------------------------------------------

void OnNodeInfo(void* data, const pw_node_info* info)
{
    auto* entry = static_cast<PipeWireClient::Impl::NodeEntry*>(data);

    if (info->props)
    {
        if (const char* name = spa_dict_lookup(info->props, PW_KEY_NODE_NAME))
            entry->info.name = name;

        if (const char* desc = spa_dict_lookup(info->props, PW_KEY_NODE_DESCRIPTION))
            entry->info.description = desc;

        if (entry->info.description.empty())
            entry->info.description = entry->info.name;
    }

    // Ask for the node's current volume/mute once we have a proxy up
    // and listening, and subscribe so future changes (including ones
    // made by *other* applications, e.g. a media player changing its
    // own stream volume doesn't count, but another mixer changing
    // this same device's volume does) keep this snapshot live.
    std::uint32_t ids[1] = { SPA_PARAM_Props };
    pw_node_subscribe_params(reinterpret_cast<pw_node*>(entry->proxy), ids, 1);
    pw_node_enum_params(reinterpret_cast<pw_node*>(entry->proxy), 0, SPA_PARAM_Props, 0, UINT32_MAX, nullptr);

    entry->owner->NotifyChange();
}

void OnNodeParam(void* data, int /*seq*/, std::uint32_t id, std::uint32_t /*index*/, std::uint32_t /*next*/, const spa_pod* param)
{
    if (id != SPA_PARAM_Props || !param)
        return;

    auto* entry = static_cast<PipeWireClient::Impl::NodeEntry*>(data);

    const auto* obj = reinterpret_cast<const spa_pod_object*>(param);
    const spa_pod_prop* prop;

    bool haveVolumes = false;
    float volumes[SPA_AUDIO_MAX_CHANNELS];
    std::uint32_t volumeCount = 0;

    SPA_POD_OBJECT_FOREACH(obj, prop)
    {
        if (prop->key == SPA_PROP_mute)
        {
            bool muted = false;
            if (spa_pod_get_bool(&prop->value, &muted) >= 0)
                entry->info.muted = muted;
        }
        else if (prop->key == SPA_PROP_channelVolumes)
        {
            volumeCount = spa_pod_copy_array(&prop->value, SPA_TYPE_Float, volumes, SPA_AUDIO_MAX_CHANNELS);
            haveVolumes = volumeCount > 0;
        }
    }

    if (haveVolumes)
    {
        entry->info.perChannelVolumes.assign(volumes, volumes + volumeCount);

        float sum = 0.0f;
        for (std::uint32_t i = 0; i < volumeCount; ++i)
            sum += volumes[i];

        entry->info.volume = sum / static_cast<float>(volumeCount);
    }

    entry->owner->NotifyChange();
}

const pw_node_events kNodeEvents = []
{
    pw_node_events events{};
    events.version = PW_VERSION_NODE_EVENTS;
    events.info = OnNodeInfo;
    events.param = OnNodeParam;
    return events;
}();

// --- metadata event callbacks ------------------------------------------------

int OnMetadataProperty(void* data, std::uint32_t subject, const char* key, const char* /*type*/, const char* value)
{
    auto* impl = static_cast<PipeWireClient::Impl*>(data);

    if (subject != kDefaultMetadataSubject || !key)
        return 0;

    if (std::strcmp(key, "default.audio.sink") == 0)
    {
        impl->defaultSinkName = ExtractJsonNameField(value);
        impl->NotifyChange();
    }
    else if (std::strcmp(key, "default.audio.source") == 0)
    {
        impl->defaultSourceName = ExtractJsonNameField(value);
        impl->NotifyChange();
    }

    return 0;
}

const pw_metadata_events kMetadataEvents = []
{
    pw_metadata_events events{};
    events.version = PW_VERSION_METADATA_EVENTS;
    events.property = OnMetadataProperty;
    return events;
}();

// --- registry event callbacks ------------------------------------------------

void OnRegistryGlobal(
    void* data, std::uint32_t id, std::uint32_t /*permissions*/,
    const char* type, std::uint32_t /*version*/, const spa_dict* props)
{
    auto* impl = static_cast<PipeWireClient::Impl*>(data);

    if (std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0 && props)
    {
        const char* mediaClass = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
        if (!mediaClass)
            return;

        bool isSink = std::strcmp(mediaClass, "Audio/Sink") == 0;
        bool isSource = std::strcmp(mediaClass, "Audio/Source") == 0;
        if (!isSink && !isSource)
            return;

        void* rawProxy = pw_registry_bind(impl->registry, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0);
        if (!rawProxy)
            return;

        auto entry = std::make_unique<PipeWireClient::Impl::NodeEntry>();
        entry->id = id;
        entry->proxy = static_cast<pw_proxy*>(rawProxy);
        entry->owner = impl;
        entry->info.id = id;
        entry->info.isSource = isSource;

        if (const char* name = spa_dict_lookup(props, PW_KEY_NODE_NAME))
            entry->info.name = name;
        if (const char* desc = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION))
            entry->info.description = desc;
        if (entry->info.description.empty())
            entry->info.description = entry->info.name;

        pw_node_add_listener(reinterpret_cast<pw_node*>(entry->proxy), &entry->listener, &kNodeEvents, entry.get());

        impl->nodes.push_back(std::move(entry));
        impl->NotifyChange();
        return;
    }

    if (std::strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0 && props)
    {
        const char* name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
        if (!name || std::strcmp(name, "default") != 0)
            return;

        void* rawProxy = pw_registry_bind(impl->registry, id, PW_TYPE_INTERFACE_Metadata, PW_VERSION_METADATA, 0);
        if (!rawProxy)
            return;

        impl->defaultMetadata = static_cast<pw_metadata*>(rawProxy);
        impl->defaultMetadataId = id;
        pw_metadata_add_listener(impl->defaultMetadata, &impl->metadataListener, &kMetadataEvents, impl);
    }
}

void OnRegistryGlobalRemove(void* data, std::uint32_t id)
{
    auto* impl = static_cast<PipeWireClient::Impl*>(data);

    for (auto it = impl->nodes.begin(); it != impl->nodes.end(); ++it)
    {
        if ((*it)->id != id)
            continue;

        pw_proxy_destroy((*it)->proxy);
        impl->nodes.erase(it);
        impl->NotifyChange();
        return;
    }

    if (id == impl->defaultMetadataId)
    {
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(impl->defaultMetadata));
        impl->defaultMetadata = nullptr;
        impl->defaultMetadataId = SPA_ID_INVALID;
    }
}

const pw_registry_events kRegistryEvents = []
{
    pw_registry_events events{};
    events.version = PW_VERSION_REGISTRY_EVENTS;
    events.global = OnRegistryGlobal;
    events.global_remove = OnRegistryGlobalRemove;
    return events;
}();

// --- metering stream callbacks -----------------------------------------------

void OnMeterProcess(void* data)
{
    auto* meter = static_cast<PipeWireClient::Impl::MeterStream*>(data);

    pw_buffer* buffer = pw_stream_dequeue_buffer(meter->stream);
    if (!buffer)
        return;

    spa_buffer* spaBuffer = buffer->buffer;
    if (spaBuffer->n_datas > 0 && spaBuffer->datas[0].data)
    {
        const auto* samples = static_cast<const float*>(spaBuffer->datas[0].data);
        std::uint32_t sampleCount = spaBuffer->datas[0].chunk->size / sizeof(float);

        float peak = 0.0f;
        for (std::uint32_t i = 0; i < sampleCount; ++i)
            peak = std::max(peak, std::fabs(samples[i]));

        meter->peak.store(peak, std::memory_order_relaxed);
    }

    pw_stream_queue_buffer(meter->stream, buffer);
}

const pw_stream_events kMeterStreamEvents = []
{
    pw_stream_events events{};
    events.version = PW_VERSION_STREAM_EVENTS;
    events.process = OnMeterProcess;
    return events;
}();

pw_stream* CreateMeterStream(pw_core* core, PipeWireClient::Impl::MeterStream* meter, const std::string& targetNodeName, bool captureSink)
{
    if (targetNodeName.empty())
        return nullptr;

    pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Monitor",
        PW_KEY_MEDIA_ROLE, "DSP",
        PW_KEY_APP_NAME, "kohiko-audio",
        PW_KEY_TARGET_OBJECT, targetNodeName.c_str(),
        nullptr
    );

    if (captureSink)
        pw_properties_set(props, PW_KEY_STREAM_CAPTURE_SINK, "true");

    pw_stream* stream = pw_stream_new(core, "kohiko-audio-meter", props);
    if (!stream)
        return nullptr;

    pw_stream_add_listener(stream, &meter->listener, &kMeterStreamEvents, meter);

    std::uint8_t buffer[1024];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

    spa_audio_info_raw formatInfo{};
    formatInfo.format = SPA_AUDIO_FORMAT_F32;
    const spa_pod* params[1];
    params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &formatInfo);

    pw_stream_flags flags = static_cast<pw_stream_flags>(
        PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS
    );

    if (pw_stream_connect(stream, PW_DIRECTION_INPUT, PW_ID_ANY, flags, params, 1) < 0)
    {
        pw_stream_destroy(stream);
        return nullptr;
    }

    return stream;
}

}

PipeWireClient::PipeWireClient() : m_impl(std::make_unique<Impl>())
{
}

PipeWireClient::~PipeWireClient()
{
    Disconnect();
}

bool PipeWireClient::Connect()
{
    static bool initialized = false;
    if (!initialized)
    {
        int argc = 0;
        pw_init(&argc, nullptr);
        initialized = true;
    }

    m_impl->loop = pw_loop_new(nullptr);
    if (!m_impl->loop)
        return false;

    m_impl->context = pw_context_new(m_impl->loop, nullptr, 0);
    if (!m_impl->context)
    {
        pw_loop_destroy(m_impl->loop);
        m_impl->loop = nullptr;
        return false;
    }

    m_impl->core = pw_context_connect(m_impl->context, nullptr, 0);
    if (!m_impl->core)
    {
        pw_context_destroy(m_impl->context);
        pw_loop_destroy(m_impl->loop);
        m_impl->context = nullptr;
        m_impl->loop = nullptr;
        return false;
    }

    m_impl->registry = pw_core_get_registry(m_impl->core, PW_VERSION_REGISTRY, 0);
    if (!m_impl->registry)
    {
        pw_core_disconnect(m_impl->core);
        pw_context_destroy(m_impl->context);
        pw_loop_destroy(m_impl->loop);
        m_impl->core = nullptr;
        m_impl->context = nullptr;
        m_impl->loop = nullptr;
        return false;
    }

    pw_registry_add_listener(m_impl->registry, &m_impl->registryListener, &kRegistryEvents, m_impl.get());

    m_impl->outputMeter.owner = m_impl.get();
    m_impl->inputMeter.owner = m_impl.get();

    m_impl->available = true;
    return true;
}

void PipeWireClient::Disconnect()
{
    StopMetering();

    m_impl->nodes.clear();

    if (m_impl->defaultMetadata)
    {
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(m_impl->defaultMetadata));
        m_impl->defaultMetadata = nullptr;
    }

    if (m_impl->core)
    {
        pw_core_disconnect(m_impl->core);
        m_impl->core = nullptr;
    }

    if (m_impl->context)
    {
        pw_context_destroy(m_impl->context);
        m_impl->context = nullptr;
    }

    if (m_impl->loop)
    {
        pw_loop_destroy(m_impl->loop);
        m_impl->loop = nullptr;
    }

    m_impl->available = false;
}

bool PipeWireClient::Available() const
{
    return m_impl->available;
}

int PipeWireClient::Fd() const
{
    if (!m_impl->available)
        return -1;

    return pw_loop_get_fd(m_impl->loop);
}

void PipeWireClient::Iterate()
{
    if (!m_impl->available)
        return;

    pw_loop_iterate(m_impl->loop, 0);
}

const std::vector<AudioNode>& PipeWireClient::Nodes() const
{
    return m_impl->snapshot;
}

void PipeWireClient::SetDefaultNode(std::uint32_t nodeId)
{
    if (!m_impl->available || !m_impl->defaultMetadata)
        return;

    auto* entry = m_impl->FindNode(nodeId);
    if (!entry)
        return;

    const char* key = entry->info.isSource ? "default.audio.source" : "default.audio.sink";
    std::string value = "{ \"name\": \"" + entry->info.name + "\" }";

    pw_metadata_set_property(m_impl->defaultMetadata, kDefaultMetadataSubject, key, "Spa:String:JSON", value.c_str());
}

void PipeWireClient::SetVolume(std::uint32_t nodeId, float linear)
{
    if (!m_impl->available)
        return;

    auto* entry = m_impl->FindNode(nodeId);
    if (!entry)
        return;

    linear = ClampVolume(linear);

    std::uint32_t channelCount = static_cast<std::uint32_t>(entry->info.perChannelVolumes.size());
    if (channelCount == 0)
        channelCount = 2;

    float volumes[SPA_AUDIO_MAX_CHANNELS];
    for (std::uint32_t i = 0; i < channelCount && i < SPA_AUDIO_MAX_CHANNELS; ++i)
        volumes[i] = linear;

    std::uint8_t buffer[512];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

    spa_pod_frame frame;
    spa_pod_builder_push_object(&builder, &frame, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
    spa_pod_builder_prop(&builder, SPA_PROP_channelVolumes, 0);
    spa_pod_builder_array(&builder, sizeof(float), SPA_TYPE_Float, channelCount, volumes);
    const spa_pod* param = static_cast<const spa_pod*>(spa_pod_builder_pop(&builder, &frame));

    pw_node_set_param(reinterpret_cast<pw_node*>(entry->proxy), SPA_PARAM_Props, 0, param);
}

void PipeWireClient::SetMute(std::uint32_t nodeId, bool muted)
{
    if (!m_impl->available)
        return;

    auto* entry = m_impl->FindNode(nodeId);
    if (!entry)
        return;

    std::uint8_t buffer[256];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

    spa_pod_frame frame;
    spa_pod_builder_push_object(&builder, &frame, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
    spa_pod_builder_prop(&builder, SPA_PROP_mute, 0);
    spa_pod_builder_bool(&builder, muted);
    const spa_pod* param = static_cast<const spa_pod*>(spa_pod_builder_pop(&builder, &frame));

    pw_node_set_param(reinterpret_cast<pw_node*>(entry->proxy), SPA_PARAM_Props, 0, param);
}

void PipeWireClient::AdjustVolume(std::uint32_t nodeId, float deltaLinear)
{
    auto* entry = m_impl->FindNode(nodeId);
    if (!entry)
        return;

    SetVolume(nodeId, entry->info.volume + deltaLinear);
}

float PipeWireClient::OutputPeakLevel() const
{
    return m_impl->outputMeter.peak.load(std::memory_order_relaxed);
}

float PipeWireClient::InputPeakLevel() const
{
    return m_impl->inputMeter.peak.load(std::memory_order_relaxed);
}

void PipeWireClient::StartMetering()
{
    if (!m_impl->available)
        return;

    if (!m_impl->outputMeter.stream)
        m_impl->outputMeter.stream = CreateMeterStream(m_impl->core, &m_impl->outputMeter, m_impl->defaultSinkName, true);

    if (!m_impl->inputMeter.stream)
        m_impl->inputMeter.stream = CreateMeterStream(m_impl->core, &m_impl->inputMeter, m_impl->defaultSourceName, false);
}

void PipeWireClient::StopMetering()
{
    if (m_impl->outputMeter.stream)
    {
        pw_stream_destroy(m_impl->outputMeter.stream);
        m_impl->outputMeter.stream = nullptr;
        m_impl->outputMeter.peak.store(0.0f, std::memory_order_relaxed);
    }

    if (m_impl->inputMeter.stream)
    {
        pw_stream_destroy(m_impl->inputMeter.stream);
        m_impl->inputMeter.stream = nullptr;
        m_impl->inputMeter.peak.store(0.0f, std::memory_order_relaxed);
    }
}

void PipeWireClient::SetChangeHandler(ChangeHandler handler)
{
    m_impl->changeHandler = std::move(handler);
}

}
