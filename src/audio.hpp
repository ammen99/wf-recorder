#ifndef AUDIO_HPP
#define AUDIO_HPP

#include <stdlib.h>
#include <stdint.h>

struct AudioReaderParams
{
    size_t audio_frame_size;
    uint32_t sample_rate;
    /* Can be NULL */
    char *audio_source;
};

class AudioReader
{
public:
    virtual ~AudioReader() {}
    virtual bool init() = 0;
    virtual void start() = 0;
    AudioReaderParams params;
    static AudioReader *create(AudioReaderParams params);
};

#endif /* end of include guard: AUDIO_HPP */
