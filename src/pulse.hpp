#pragma once
#include <config.h>

struct PulseReaderParams
{
    /* Can be NULL */
    char *audio_source;
};

#ifdef HAVE_PULSE
#include <pulse/simple.h>
#include <pulse/error.h>
#include <thread>

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
#else // => !HAVE_PULSE

// NO-OP
class PulseReader
{
  public:
    PulseReader(PulseReaderParams) {}
    ~PulseReader() {}
    void start() {}
};

#endif // HAVE_PULSE
