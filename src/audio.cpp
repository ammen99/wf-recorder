#include "audio.hpp"
#include "config.h"

#ifdef HAVE_PULSE
#include "pulse.hpp"
#endif

AudioReader *AudioReader::create(AudioReaderParams params)
{
    AudioReader *pa = new PulseReader;
    pa->params = params;
    pa->init();
    return pa;
}
