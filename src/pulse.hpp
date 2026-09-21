#ifndef PULSE_HPP
#define PULSE_HPP

#include "audio.hpp"

#include <pulse/simple.h>
#include <pulse/error.h>
#include <thread>
#include <atomic>

class PulseReader : public AudioReader
{
    pa_simple *pa = nullptr;
    pa_sample_spec sample_spec;

    bool loop();
    std::thread read_thread;
    std::atomic<uint64_t> monotonic_clock_start{0};

    public:
    ~PulseReader();

    bool init() override;
    void start() override;
    uint64_t get_time_base() const override;
};

#endif /* end of include guard: PULSE_HPP */
