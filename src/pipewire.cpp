#include "pipewire.hpp"
#include "frame-writer.hpp"
#include <iostream>
#include <spa/param/audio/format-utils.h>

PipeWireReader::~PipeWireReader()
{
    pw_thread_loop_lock(thread_loop);
    if (stream) {
        spa_hook_remove(&stream_listener);
        if (pw_stream_get_state(stream, nullptr) != PW_STREAM_STATE_UNCONNECTED)
            pw_stream_disconnect(stream);
        pw_stream_destroy(stream);
    }
    pw_thread_loop_unlock(thread_loop);
    pw_thread_loop_stop(thread_loop);
    if (core) {
        spa_hook_remove(&core_listener);
        pw_core_disconnect(core);
    }
    if (context)
        pw_context_destroy(context);
    pw_thread_loop_destroy(thread_loop);
    pw_deinit();

    delete []buf;
}

static void on_core_done(void *data, uint32_t id, int seq)
{
    PipeWireReader *pr = static_cast<PipeWireReader*>(data);

    if (id == PW_ID_CORE && pr->seq == seq)
        pw_thread_loop_signal(pr->thread_loop, false);
}

static void on_core_error(void *data, uint32_t, int, int res, const char *message)
{
    PipeWireReader *pr = static_cast<PipeWireReader*>(data);

    std::cerr << "pipewire: core error " << res << " " << message << std::endl;
    pw_thread_loop_signal(pr->thread_loop, false);
}

static const struct pw_core_events core_events = {
    .version = PW_VERSION_CORE_EVENTS,
    .info = nullptr,
    .done = on_core_done,
    .ping = nullptr,
    .error = on_core_error,
    .remove_id = nullptr,
    .bound_id = nullptr,
    .add_mem = nullptr,
    .remove_mem = nullptr,
    .bound_props = nullptr,
};

bool PipeWireReader::init()
{
    buf = new uint8_t[params.audio_frame_size * 4];

    int argc = 0;
    pw_init(&argc, nullptr);

    thread_loop = pw_thread_loop_new("PipeWire", nullptr);
    context = pw_context_new(pw_thread_loop_get_loop(thread_loop), nullptr, 0);
    if (!context) {
        std::cerr << "pipewire: context_new error" << std::endl;
        return false;
    }

    pw_thread_loop_lock(thread_loop);

    if (pw_thread_loop_start(thread_loop) < 0) {
        std::cerr << "pipewire: thread_loop_start error" << std::endl;
        pw_thread_loop_unlock(thread_loop);
        return false;
    }

    core = pw_context_connect(context, nullptr, 0);
    if (!core) {
        std::cerr << "pipewire: context_connect error" << std::endl;
        pw_thread_loop_unlock(thread_loop);
        return false;
    }
    pw_core_add_listener(core, &core_listener, &core_events, this);

    return true;
}

static void on_stream_process(void *data)
{
    PipeWireReader *pr = static_cast<PipeWireReader*>(data);

    struct pw_buffer *b = pw_stream_dequeue_buffer(pr->stream);
    if (!b) {
        std::cerr << "pipewire: out of buffers: " << strerror(errno) << std::endl;
        return;
    }

    for (uint32_t i = 0; i < b->buffer->n_datas; ++i) {
        struct spa_data *d = &b->buffer->datas[i];
        memcpy(pr->buf + pr->buf_size, d->data, d->chunk->size);
        pr->buf_size += d->chunk->size;
        while (pr->buf_size >= pr->params.audio_frame_size) {
            frame_writer->add_audio(pr->buf);
            pr->buf_size -= pr->params.audio_frame_size;
            if (pr->buf_size)
                memmove(pr->buf, pr->buf + pr->params.audio_frame_size, pr->buf_size);
        }
    }

    if (!pr->time_base)
        pr->time_base = b->time;

    pw_stream_queue_buffer(pr->stream, b);
}

static const struct pw_stream_events stream_events = {
    .version = PW_VERSION_STREAM_EVENTS,
    .destroy = nullptr,
    .state_changed = nullptr,
    .control_info = nullptr,
    .io_changed = nullptr,
    .param_changed = nullptr,
    .add_buffer = nullptr,
    .remove_buffer = nullptr,
    .process = on_stream_process,
    .drained = nullptr,
    .command = nullptr,
    .trigger_done = nullptr,
};

static void on_registry_global(void *data, uint32_t, uint32_t, const char *type, uint32_t, const struct spa_dict *props)
{
    PipeWireReader *pr = static_cast<PipeWireReader*>(data);

    if (strcmp(type, PW_TYPE_INTERFACE_Node) != 0)
        return;

    const char *name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    if (!name || strcmp(pr->params.audio_source, name) != 0)
        return;

    const char *media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    if (!media_class)
        return;

    pr->source_found = true;
    pr->source_is_sink = strcmp(media_class, "Audio/Sink") == 0;
}

static const struct pw_registry_events registry_events = {
    .version = PW_VERSION_REGISTRY_EVENTS,
    .global = on_registry_global,
    .global_remove = nullptr,
};

void PipeWireReader::start()
{
    struct pw_properties *props =
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio",
                          PW_KEY_MEDIA_CATEGORY, "Capture",
                          PW_KEY_MEDIA_ROLE, "Screen",
                          PW_KEY_STREAM_CAPTURE_SINK, "true",
                          PW_KEY_NODE_NAME, "wf-recorder",
                          NULL);

    if (params.audio_source) {
        struct pw_registry *registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
        if (registry) {
            struct spa_hook registry_listener;
            pw_registry_add_listener(registry, &registry_listener, &registry_events, this);
            seq = pw_core_sync(core, PW_ID_CORE, seq);
            pw_thread_loop_wait(thread_loop);
            if (!source_found) {
                std::cerr << "pipewire: source " << params.audio_source << " not found, using default" << std::endl;
            } else {
                pw_properties_set(props, PW_KEY_STREAM_CAPTURE_SINK, source_is_sink ? "true" : "false");
                pw_properties_set(props, PW_KEY_TARGET_OBJECT, params.audio_source);
            }
            spa_hook_remove(&registry_listener);
            pw_proxy_destroy(reinterpret_cast<struct pw_proxy*>(registry));
        }
    }

    stream = pw_stream_new(core, "wf-recorder", props);
    pw_stream_add_listener(stream, &stream_listener, &stream_events, this);

    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

    struct spa_audio_info_raw info = {};
    info.format = SPA_AUDIO_FORMAT_F32_LE;
    info.rate = params.sample_rate;
    info.channels = 2;
    const struct spa_pod *audio_param = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);

    pw_stream_connect(stream,
                      PW_DIRECTION_INPUT,
                      PW_ID_ANY,
                      static_cast<enum pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                                        PW_STREAM_FLAG_DONT_RECONNECT |
                                                        PW_STREAM_FLAG_MAP_BUFFERS),
                      &audio_param, 1);

    pw_thread_loop_unlock(thread_loop);
}

uint64_t PipeWireReader::get_time_base() const
{
    return time_base / 1000;
}
