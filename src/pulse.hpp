#ifndef PULSE_HPP
#define PULSE_HPP

#include "audio.hpp"

#include <pulse/simple.h>
#include <pulse/error.h>
#include <thread>

class PulseReader : public AudioReader
{
    pa_simple *pa;

    bool loop();
    std::thread read_thread;

    public:
    ~PulseReader();

    bool init() override;
    void start() override;
};

#endif /* end of include guard: PULSE_HPP */
