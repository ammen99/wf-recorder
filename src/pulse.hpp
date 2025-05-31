#ifndef PULSE_HPP
#define PULSE_HPP

#include "audio.hpp"

#include <pulse/simple.h>
#include <pulse/error.h>
#include <thread>
#include <vector>

struct PulseSource
{
    pa_simple *pa;
    std::thread read_thread;
    std::string source_name;
    int stream_index = 0;  // Index of the output stream for this source
};

class PulseReader : public AudioReader
{
    std::vector<PulseSource> sources;
    uint64_t monotonic_clock_start = 0;
    bool loop(PulseSource &source);

    public:
    ~PulseReader();

    bool init() override;
    void start() override;
    uint64_t get_time_base() const override;
};

#endif /* end of include guard: PULSE_HPP */
