#include "pulse.hpp"
#include "frame-writer.hpp"
#include <iostream>
#include <vector>
#include <cstring>
#include <thread>
#include <map>
#include "debug.hpp"

bool PulseReader::init()
{
    if (params.audio_sources.empty()) {
        // Add default audio source
        params.audio_sources.push_back("");
    }

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

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    this->monotonic_clock_start = ts.tv_sec * 1000000ll + ts.tv_nsec / 1000ll;

    bool at_least_one_success = false;

    for (const auto& source_name : params.audio_sources)
    {
        int perr;
        const char* source_cstr = source_name.empty() ? nullptr : source_name.c_str();
        std::cerr << "Using PulseAudio device: " << (source_cstr ?: "default") << std::endl;

        // Debug logging for arguments
        dbg << "DEBUG: PulseAudio connection params:" << std::endl;
        dbg << "  - Server: " << (NULL ? "null" : "null") << std::endl;
        dbg << "  - Client name: wf-recorder" << std::endl;
        dbg << "  - Stream direction: PA_STREAM_RECORD (" << PA_STREAM_RECORD << ")" << std::endl;
        dbg << "  - Source name: " << (source_cstr ? source_cstr : "nullptr") << std::endl;
        dbg << "  - Stream name: wf-recorder" << std::endl;
        dbg << "  - Sample format: " << sample_spec.format << " (PA_SAMPLE_FLOAT32LE=" << PA_SAMPLE_FLOAT32LE << ")" << std::endl;
        dbg << "  - Sample rate: " << sample_spec.rate << std::endl;
        dbg << "  - Channels: " << (int)sample_spec.channels << std::endl;
        dbg << "  - Buffer maxlength: " << attr.maxlength << std::endl;
        dbg << "  - Buffer fragsize: " << attr.fragsize << std::endl;

        PulseSource source;
        source.source_name = source_name;
        source.pa = pa_simple_new(NULL, "wf-recorder", PA_STREAM_RECORD, source_cstr,
            "wf-recorder", &sample_spec, &map, &attr, &perr);

        if (!source.pa)
        {
            std::cerr << "Failed to connect to PulseAudio source " << (source_cstr ?: "default") << ": "
                << pa_strerror(perr) << " (error code: " << perr << ")" << std::endl;
            continue;
        }

        // Set the stream index for this source
        source.stream_index = sources.size();
        dbg << "DEBUG: Assigning stream index " << source.stream_index
                  << " to source '" << (source_cstr ?: "default") << "'" << std::endl;

        int error = 0;
        uint64_t latency_audio = pa_simple_get_latency(source.pa, &error);
        if (latency_audio != (pa_usec_t)-1 && !at_least_one_success) {
            monotonic_clock_start -= latency_audio;
        }

        sources.push_back(std::move(source));
        at_least_one_success = true;
    }

    if (!at_least_one_success)
    {
        std::cerr << "Failed to connect to any PulseAudio sources. Recording won't have audio." << std::endl;
        return false;
    }

    return true;
}

bool PulseReader::loop(PulseSource &source)
{
    static std::vector<char> buffer;
    buffer.resize(params.audio_frame_size);

    int perr;
    if (pa_simple_read(source.pa, buffer.data(), buffer.size(), &perr) < 0)
    {
        std::cerr << "Failed to read from PulseAudio stream (" << (source.source_name.empty() ? "default" : source.source_name)
            << "): " << pa_strerror(perr) << std::endl;
        return false;
    }

    // Use the stream index assigned by main.cpp
    extern std::map<std::string, int> audio_source_streams;

    int stream_index = 0; // Default to 0
    auto it = audio_source_streams.find(source.source_name);
    if (it != audio_source_streams.end()) {
        stream_index = it->second;
    }

    // Extra logging to debug stream assignment
    dbg << "DEBUG: Writing audio data from source '"
              << (source.source_name.empty() ? "default" : source.source_name)
              << "' to stream index " << stream_index << std::endl;

    // Pass the source index to add_audio
    frame_writer->add_audio(buffer.data(), buffer.size(), stream_index);
    return !exit_main_loop;
}

void PulseReader::start()
{
    if (sources.empty())
        return;

    for (auto& source : sources)
    {
        if (!source.pa)
            continue;

        source.read_thread = std::thread([this, &source] ()
        {
            while (loop(source));
        });
    }
}

PulseReader::~PulseReader()
{
    for (auto& source : sources)
    {
        if (source.pa)
        {
            if (source.read_thread.joinable())
                source.read_thread.join();
            pa_simple_free(source.pa);
        }
    }
}

uint64_t PulseReader::get_time_base() const
{
    return monotonic_clock_start;
}
