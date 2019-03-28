#ifndef PULSE_HPP
#define PULSE_HPP

#include <pulse/simple.h>
#include <pulse/error.h>
#include <thread>

struct PulseReaderParams
{
    size_t audio_frame_size;
    /* Can be NULL */
    char *audio_source;
};

class PulseReader
{
    PulseReaderParams params;
    pa_simple *pa;

    bool loop();
    std::thread read_thread;

    public:
    PulseReader(PulseReaderParams params);
    ~PulseReader();

    void start();
};

#endif /* end of include guard: PULSE_HPP */
