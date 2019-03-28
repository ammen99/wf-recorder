#include "pulse.hpp"
#include "frame-writer.hpp"
#include <iostream>
#include <vector>
#include <cstring>
#include <thread>

static uint64_t timespec_to_usec (const timespec& ts)
{
    return ts.tv_sec * 1000000ll + 1ll * ts.tv_nsec / 1000ll;
}

PulseReader::PulseReader(PulseReaderParams _p)
    : params(_p)
{

    pa_channel_map map;
    std::memset(&map, 0, sizeof(map));
    pa_channel_map_init_stereo(&map);

    pa_buffer_attr attr;
    attr.maxlength = params.audio_frame_size * 4;
    attr.fragsize  = params.audio_frame_size * 4;

    pa_sample_spec sample_spec =
    {
        .format = PA_SAMPLE_FLOAT32LE,
        .rate = 44100,
        .channels = 2,
    };

    int perr;
    pa = pa_simple_new(NULL, "wf-recorder", PA_STREAM_RECORD, NULL,
        "wf-recorder", &sample_spec, &map, &attr, &perr);

    if (!pa)
    {
        std::cerr << "Failed to connect to PulseAudio,"
            << " recording won't have audio" << std::endl;
    }
}

bool PulseReader::loop()
{
    static std::vector<char> buffer;
    buffer.resize(params.audio_frame_size);

    int perr;
    if (pa_simple_read(pa, buffer.data(), buffer.size(), &perr) < 0)
    {
        std::cerr << "Failed to read from PulseAudio stream!" << std::endl;
        return false;
    }

    frame_writer->add_audio(buffer.data());
    return !exit_main_loop;
}

void PulseReader::start()
{
    if (!pa)
        return;

    read_thread = std::thread([=] ()
    {
        while (loop());
    });
}

PulseReader::~PulseReader()
{
    if (pa)
        read_thread.join();
}