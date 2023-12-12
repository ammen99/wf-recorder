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
    if (getenv("WF_RECORDER_PIPEWIRE")) {
        AudioReader *pw = new PipeWireReader;
        pw->params = params;
        if (pw->init())
            return pw;
        delete pw;
    }
#endif
#ifdef HAVE_PULSE
    AudioReader *pa = new PulseReader;
    pa->params = params;
    if (pa->init())
        return pa;
    delete pa;
#endif
    return nullptr;
}
