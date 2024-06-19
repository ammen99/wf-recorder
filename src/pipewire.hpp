#ifndef PIPEWIRE_HPP
#define PIPEWIRE_HPP

#include "audio.hpp"

#include <pipewire/pipewire.h>

class PipeWireReader : public AudioReader
{
public:
    ~PipeWireReader();
    bool init() override;
    void start() override;
    uint64_t get_time_base() const override;

    struct pw_thread_loop *thread_loop = nullptr;
    struct pw_context *context = nullptr;
    struct pw_core *core = nullptr;
    struct spa_hook core_listener;
    struct pw_stream *stream = nullptr;
    struct spa_hook stream_listener;

    int seq = 0;
    bool source_found = false;
    bool source_is_sink = false;

    uint8_t *buf = nullptr;
    size_t buf_size = 0;

    uint64_t time_base = 0;
};

#endif /* end of include guard: PIPEWIRE_HPP */
