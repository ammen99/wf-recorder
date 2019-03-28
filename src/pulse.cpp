#include "pulse.hpp"
#include "frame-writer.hpp"
#include <iostream>
#include <cstring>
#include <chrono>
#include <thread>

static uint64_t timespec_to_usec (const timespec& ts)
{
    return ts.tv_sec * 1000000ll + 1ll * ts.tv_nsec / 1000ll;
}

static void pulse_stream_read_cb(pa_stream *stream, size_t size, void *data)
{
    auto reader = static_cast<PulseReader*> (data);

    const void *buffer;
    pa_stream_peek(stream, &buffer, &size);

    int64_t time;

    int delay_neg;
    pa_usec_t pts, delay;
    if (!pa_stream_get_time(stream, &pts) && !pa_stream_get_latency(stream, &delay, &delay_neg))
    {
        //std::cout << "packet: " << pts << " " << delay << " " << delay_neg << std::endl;
        time = pts;
        time -= delay_neg ? -delay : delay;
    } else
    {
        //std::cout << "got shit" << std::endl;
        time = INT64_MIN;
    }

    time += reader->audio_delay;

//    std::cout << "time is " << timespec_to_usec(get_ct()) << std::endl;
    std::cout << "audio fr: " << time / 1000000.0 << std::endl;

    frame_writer_pending_mutex.unlock();
    if (frame_writer)
        frame_writer->add_audio(buffer, size, time);

   // frame_writer_mutex.unlock();
    pa_stream_drop(stream);

    if (exit_main_loop)
        pa_stream_disconnect(stream);
}

static void pulse_stream_status_cb(pa_stream *stream, void *data)
{
    auto reader = static_cast<PulseReader*> (data);
    switch (pa_stream_get_state(stream))
    {
        case PA_STREAM_UNCONNECTED:
            std::cout << "pa stream unconnected" << std::endl;
            break;
        case PA_STREAM_CREATING:
            std::cout << "pa stream being created" << std::endl;
            break;
        case PA_STREAM_READY:
            std::cout << "pa stream ready" << std::endl;
            break;
        case PA_STREAM_FAILED:
            std::cout << "pa stream failed" << std::endl;
            break;
        case PA_STREAM_TERMINATED:
            std::cout << "pa stream terminated" << std::endl;
            pa_context_disconnect(reader->context);
            break;
    }
}

static void pulse_stream_buffer_attr_cb(pa_stream *, void *) { }
static void pulse_stream_overflow_cb(pa_stream *, void *) { std::cout << "overflow" << std::endl; }
static void pulse_stream_underflow_cb(pa_stream *, void *) { std::cout << "underflow" << std::endl;}

static void pulse_sink_info_cb(pa_context *context, const pa_sink_info *info,
                               int is_last, void *data)
{
    auto reader = static_cast<PulseReader*> (data);
    if (is_last)
        return;

    std::cout << "start pulseaudio capture from " << info->name << std::endl;

    pa_stream *stream;
    pa_buffer_attr attr;
    pa_channel_map map;

    std::memset(&attr, 0, sizeof(attr));
    std::memset(&map, 0, sizeof(map));

    reader->spec.format   = PA_SAMPLE_FLOAT32LE;
    reader->spec.rate     = 44100;
    reader->spec.channels = 2;
    pa_channel_map_init_stereo(&map);

    attr.maxlength = -1;
    attr.fragsize  = -1;

    stream = pa_stream_new(context, "wf-recorder", &reader->spec, &map);

    /* Set stream callbacks */
    pa_stream_set_state_callback(stream, pulse_stream_status_cb, data);
    pa_stream_set_read_callback(stream, pulse_stream_read_cb, data);
    pa_stream_set_underflow_callback(stream, pulse_stream_underflow_cb, data);
    pa_stream_set_overflow_callback(stream, pulse_stream_overflow_cb, data);
    pa_stream_set_buffer_attr_callback(stream, pulse_stream_buffer_attr_cb, data);

    /* Start stream */
    auto flags = PA_STREAM_ADJUST_LATENCY | PA_STREAM_AUTO_TIMING_UPDATE |
                             PA_STREAM_INTERPOLATE_TIMING | PA_STREAM_FIX_RATE |
                             PA_STREAM_FIX_CHANNELS | PA_STREAM_START_UNMUTED;

    std::cout << "start rrecord " << timespec_to_usec(get_ct()) / 1.0e6 << std::endl;
    pa_stream_connect_record(stream, info->monitor_source_name, &attr, (pa_stream_flags_t) flags);
}

static void pulse_server_info_cb(pa_context *context, const pa_server_info *info,
                                 void *data)
{
    pa_operation *op;
    op = pa_context_get_sink_info_by_name(context, info->default_sink_name,
        pulse_sink_info_cb, data);
    pa_operation_unref(op);
}

void pa_sinklist_cb(pa_context *c, const pa_sink_info *l, int eol, void *userdata)
{
    int ctr = 0;

    // If eol is set to a positive number, you're at the end of the list
    if (eol > 0) {
        return;
    }

    std::cout << "got " << l->name << "#" << l->description << std::endl;
}

static void pulse_state_cb(pa_context *context, void *data)
{
    switch (pa_context_get_state(context))
    {
        case PA_CONTEXT_UNCONNECTED:
            std::cout << "pulseaudio: unconnected" << std::endl;
            break;
        case PA_CONTEXT_CONNECTING:
            std::cout << "pulseaudio: connecting" << std::endl;
            break;
        case PA_CONTEXT_AUTHORIZING:
            std::cout << "pulseaudio: authorizing" << std::endl;
            break;
        case PA_CONTEXT_SETTING_NAME:
            std::cout << "pulseaudio: setting name" << std::endl;
            break;
        case PA_CONTEXT_FAILED:
            std::cout << "pulseaudio: connection failed" << std::endl;
            break;
        case PA_CONTEXT_TERMINATED:
            std::cout << "pulseaudio: connection terminated" << std::endl;
            break;
        case PA_CONTEXT_READY:
            std::cout << "pulseaudio: connection ready" << std::endl;
            pa_operation_unref(pa_context_get_server_info(
                    context, pulse_server_info_cb, data));
            pa_context_get_sink_info_list(context,
                pa_sinklist_cb, NULL);
            break;
    }
}

PulseReader::PulseReader(int64_t ab)
    : audio_delay(ab)
{
    mainloop = pa_threaded_mainloop_new();
    api = pa_threaded_mainloop_get_api(mainloop);

    std::cout << "start reader " << timespec_to_usec(get_ct()) / 1.0e6<< std::endl;


    context = pa_context_new(api, "wf-recorder");
    pa_context_set_state_callback(context, pulse_state_cb, this);
    pa_context_connect(context, NULL, PA_CONTEXT_NOAUTOSPAWN, NULL);
}

void PulseReader::run_loop()
{
    pa_threaded_mainloop_start(mainloop);
}

PulseReader::~PulseReader()
{
    pa_threaded_mainloop_stop(mainloop);
    pa_threaded_mainloop_free(mainloop);
//    pa_context_unref(context);
}
