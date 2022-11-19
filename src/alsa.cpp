#include "alsa.hpp"
#include "src/frame-writer.hpp"
#include <iostream>

static constexpr snd_pcm_format_t format = SND_PCM_FORMAT_S16_LE;

AlsaReader::AlsaReader(AlsaReaderParams params)
{
    this->params = params;

    if (snd_pcm_open(&handle, params.audio_device.c_str(), SND_PCM_STREAM_CAPTURE, 0) < 0)
    {
        std::cerr << "Failed to open audio device " << params.audio_device << std::endl;
        std::exit(-1);
    }

    snd_pcm_hw_params_t *hwp;
    if (snd_pcm_hw_params_malloc(&hwp) < 0)
    {
        std::cerr << "Failed to allocate hw params!" << std::endl;
        std::exit(-1);
    }

    if (snd_pcm_hw_params_any(handle, hwp) < 0)
    {
        std::cerr << "Failed to allocate hw params!" << std::endl;
        std::exit(-1);
    }

    if (snd_pcm_hw_params_set_access(handle, hwp, SND_PCM_ACCESS_RW_INTERLEAVED) < 0)
    {
        std::cerr << "Failed to set access mode!" << std::endl;
        std::exit(-1);
    }

    long frames = 2048;
    if (snd_pcm_hw_params_set_period_size(handle, hwp, frames, 0) < 0)
    {
        std::cerr << "Failed to set period size!" << std::endl;
        std::exit(-1);
    }

    if (snd_pcm_hw_params_set_format(handle, hwp, format) < 0)
    {
        std::cerr << "Failed to set audio format!" << std::endl;
        std::exit(-1);
    }

    rate = 44100;
    snd_pcm_hw_params_set_rate_resample(handle, hwp, 1);
    if (snd_pcm_hw_params_set_rate_near(handle, hwp, &rate, 0) < 0)
    {
        std::cerr << "Failed to set capture rate!" << std::endl;
        std::exit(-1);
    };

    if (snd_pcm_hw_params_set_channels(handle, hwp, 2) < 0)
    {
        std::cout << "Failed to start stereo recording!" << std::endl;
        std::exit(-1);
    };

    int err;
    if ((err = snd_pcm_hw_params(handle, hwp)) < 0)
    {
        std::cerr << "Failed to set audio parameters! " << snd_strerror(err) << std::endl;
        std::exit(-1);
    }

    snd_pcm_hw_params_free(hwp);

    if (snd_pcm_prepare(handle) < 0)
    {
        std::cerr << "Preparing audio interface for capture failed!" << std::endl;
        std::exit(-1);
    }
}

bool AlsaReader::loop()
{
    // * 2 channels * 2 bytes
    auto samples = frame_writer->get_audio_nb_samples();
    buffer.resize(samples * 2 * 2);
    if (snd_pcm_readi(handle, buffer.data(), samples) != (int)samples)
    {
        std::cerr << "Failed reading from ALSA device!" << std::endl;
        return false;
    }

    frame_writer->add_audio(buffer);
    return !exit_main_loop;
}

AlsaReader::~AlsaReader()
{
    snd_pcm_close(handle);
    read_thread.join();
}

void AlsaReader::start()
{
    read_thread = std::thread([&] ()
    {
        while(loop());
    });
}
