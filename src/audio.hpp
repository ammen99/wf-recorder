#ifndef AUDIO_HPP
#define AUDIO_HPP

#include <stdlib.h>
#include <stdint.h>
#include "config.h"
#include <string>

struct AudioReaderParams
{
    size_t audio_frame_size;
    uint32_t sample_rate;
    /* Can be NULL */
    char *audio_source;

    std::string audio_backend = DEFAULT_AUDIO_BACKEND;
};

class AudioReader
{
public:
    virtual ~AudioReader() {}
    virtual bool init() = 0;
    virtual void start() = 0;
    AudioReaderParams params;
    static AudioReader *create(AudioReaderParams params);
    virtual uint64_t get_time_base() const { return 0; }
};

#endif /* end of include guard: AUDIO_HPP */
