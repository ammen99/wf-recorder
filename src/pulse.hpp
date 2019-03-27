#ifndef PULSE_HPP
#define PULSE_HPP

#include <pulse/pulseaudio.h>

class PulseReader
{
    public:
    int64_t audio_delay;
    pa_sample_spec spec;
    pa_context *context;
    pa_threaded_mainloop *mainloop;
    pa_mainloop_api *api;

    PulseReader(int64_t audio_delay);
    ~PulseReader();
    void run_loop();
};

#endif /* end of include guard: PULSE_HPP */
