#include "pulse.hpp"
#include "frame-writer.hpp"
#include <iostream>
#include <vector>
#include <cstring>
#include <thread>

bool PulseReader::init()
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
        .rate = params.sample_rate,
        .channels = 2,
    };

    int perr;
    std::cerr << "Using PulseAudio device: " << (params.audio_source ?: "default") << std::endl;
    pa = pa_simple_new(NULL, "wf-recorder3", PA_STREAM_RECORD, params.audio_source,
        "wf-recorder3", &sample_spec, &map, &attr, &perr);

    if (!pa)
    {
        std::cerr << "Failed to connect to PulseAudio: " << pa_strerror(perr)
            << "\nRecording won't have audio" << std::endl;
        return false;
    }

    return true;
}

bool PulseReader::loop()
{
    static std::vector<char> buffer;
    buffer.resize(params.audio_frame_size);

    int perr;
    if (pa_simple_read(pa, buffer.data(), buffer.size(), &perr) < 0)
    {
        std::cerr << "Failed to read from PulseAudio stream: "
            << pa_strerror(perr) << std::endl;
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
