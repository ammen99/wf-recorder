#include "audio.hpp"
#include "config.h"

#ifdef HAVE_PULSE
#include "pulse.hpp"
#endif

#ifdef HAVE_PIPEWIRE
#include "pipewire.hpp"
#endif

AudioReader *AudioReader::create(AudioReaderParams params)
{
#ifdef HAVE_PIPEWIRE
    if (params.audio_backend == "pipewire") {
        AudioReader *pw = new PipeWireReader;
        pw->params = params;
        if (pw->init())
            return pw;
        delete pw;
    }
#endif
#ifdef HAVE_PULSE
    if (params.audio_backend == "pulse") {
        AudioReader *pa = new PulseReader;
        pa->params = params;
        if (pa->init())
            return pa;
        delete pa;
    }
#endif
    return nullptr;
}
