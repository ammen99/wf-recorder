#pragma once
#include <config.h>
#include "frame-writer.hpp"

#include <string>
struct AlsaReaderParams
{
    std::string audio_device;
};

#ifdef HAVE_PULSE
#include <thread>
#include <vector>
#include <alsa/asoundlib.h>

class AlsaReader
{
    std::vector<char> buffer;
    snd_pcm_t *handle;
    AlsaReaderParams params;

    bool loop();
    std::thread read_thread;
    unsigned int rate;

  public:
    AlsaReader(AlsaReaderParams params);
    ~AlsaReader();

    void start();
    int get_rate() { return rate; }
    int get_fmt() { return AV_SAMPLE_FMT_S16; }
};

#else // => !HAVE_PULSE

// NO-OP
class AlsaReader
{
  public:
    AlsaReader(AlsaReaderParams) {}
    ~AlsaReader() {}

    void start() {}
    int get_rate() { return 0; }
    int get_fmt() { return AV_SAMPLE_FMT_S16; }
};

#endif
