// Adapted from https://stackoverflow.com/questions/34511312/how-to-encode-a-video-from-several-images-generated-in-a-c-program-without-wri
// (Later) adapted from https://github.com/apc-llc/moviemaker-cpp
//
// Audio encoding - thanks to wlstream, a lot of the code/ideas are taken from there

#include <iostream>
#include "frame-writer.hpp"
#include <vector>
#include <queue>
#include <cstring>

#define FPS 60
#define PIX_FMT AV_PIX_FMT_YUV420P
#define AUDIO_RATE 44100

using namespace std;

class FFmpegInitialize
{
public :

    FFmpegInitialize()
    {
        // Loads the whole database of available codecs and formats.
        av_register_all();
    }
};

static FFmpegInitialize ffmpegInitialize;

void FrameWriter::init_hw_accel()
{
    int ret = av_hwdevice_ctx_create(&this->hw_device_context,
        av_hwdevice_find_type_by_name("vaapi"), params.hw_device.c_str(), NULL, 0);

    if (ret != 0)
    {
        std::cerr << "Failed to create hw encoding device " << params.hw_device << std::endl;
        std::exit(-1);
    }

    this->hw_frame_context = av_hwframe_ctx_alloc(hw_device_context);
    if (!this->hw_frame_context)
    {
        std::cerr << "Failed to initialize hw frame context" << std::endl;
        av_buffer_unref(&hw_device_context);
        std::exit(-1);
    }

    AVHWFramesConstraints *cst;
    cst = av_hwdevice_get_hwframe_constraints(hw_device_context, NULL);
    if (!cst)
    {
        std::cerr << "Failed to get hwframe constraints" << std::endl;
        av_buffer_unref(&hw_device_context);
        std::exit(-1);
    }

    AVHWFramesContext *ctx = (AVHWFramesContext*)this->hw_frame_context->data;
    ctx->width = params.width;
    ctx->height = params.height;
    ctx->format = cst->valid_hw_formats[0];
    ctx->sw_format = AV_PIX_FMT_NV12;

    if (av_hwframe_ctx_init(hw_frame_context))
    {
        std::cerr << "Failed to initialize hwframe context" << std::endl;
        av_buffer_unref(&hw_device_context);
        av_buffer_unref(&hw_frame_context);
        std::exit(-1);
    }
}

void FrameWriter::load_codec_options(AVDictionary **dict)
{
    static const std::map<std::string, std::string> default_x264_options = {
        {"tune", "zerolatency"},
        {"preset", "ultrafast"},
        {"crf", "20"},
    };

    if (!params.codec.compare("libx264") || !params.codec.compare("libx265"))
    {
        for (const auto& param : default_x264_options)
        {
            if (!params.codec_options.count(param.first))
                params.codec_options[param.first] = param.second;
        }
    }

    for (auto& opt : params.codec_options)
    {
        std::cout << "Setting codec option: " << opt.first << "=" << opt.second << std::endl;
        av_dict_set(dict, opt.first.c_str(), opt.second.c_str(), 0);
    }
}

void FrameWriter::init_video_stream()
{
    AVDictionary *options = NULL;
    load_codec_options(&options);

    AVCodec* codec = avcodec_find_encoder_by_name(params.codec.c_str());
    if (!codec)
    {
        std::cerr << "Failed to find the given codec" << std::endl;
        std::exit(-1);
    }

    videoStream = avformat_new_stream(fmtCtx, codec);
    if (!videoStream)
    {
        std::cerr << "Failed to open stream" << std::endl;
        std::exit(-1);
    }

    videoCodecCtx = videoStream->codec;
    videoCodecCtx->width = params.width;
    videoCodecCtx->height = params.height;
    videoCodecCtx->time_base = (AVRational){ 1, FPS };

    if (params.codec.find("vaapi") != std::string::npos)
    {
        videoCodecCtx->pix_fmt = AV_PIX_FMT_VAAPI;
        init_hw_accel();
        videoCodecCtx->hw_frames_ctx = av_buffer_ref(hw_frame_context);
    } else
    {
        videoCodecCtx->pix_fmt = PIX_FMT;
        init_sws();
    }

    if (fmtCtx->oformat->flags & AVFMT_GLOBALHEADER)
        videoCodecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    int err;
    if ((err = avcodec_open2(videoCodecCtx, codec, &options)) < 0)
    {
        std::cerr << "avcodec_open2 failed " << err << std::endl;
        std::exit(-1);
    }
    av_dict_free(&options);
    videoStream->time_base = (AVRational){ 1, FPS };
}

static uint64_t get_codec_channel_layout(AVCodec *codec)
{
      int i = 0;
      if (!codec->channel_layouts)
          return AV_CH_LAYOUT_STEREO;
      while (1) {
          if (!codec->channel_layouts[i])
              break;
          if (codec->channel_layouts[i] == AV_CH_LAYOUT_STEREO)
              return codec->channel_layouts[i];
          i++;
      }
      return codec->channel_layouts[0];
}

static enum AVSampleFormat get_codec_sample_fmt(AVCodec *codec)
{
    int i = 0;
    if (!codec->sample_fmts)
        return AV_SAMPLE_FMT_S16;
    while (1) {
        if (codec->sample_fmts[i] == -1)
            break;
        if (av_get_bytes_per_sample(codec->sample_fmts[i]) >= 2)
            return codec->sample_fmts[i];
        i++;
    }
    return codec->sample_fmts[0];
}

void FrameWriter::init_audio_stream()
{
    AVCodec* codec = avcodec_find_encoder_by_name("aac");
    if (!codec)
    {
        std::cerr << "Failed to find the aac codec" << std::endl;
        std::exit(-1);
    }

    audioStream = avformat_new_stream(fmtCtx, codec);
    if (!audioStream)
    {
        std::cerr << "Failed to open audio stream" << std::endl;
        std::exit(-1);
    }

    audioCodecCtx = audioStream->codec;
    audioCodecCtx->bit_rate = lrintf(128000.0f);
    audioCodecCtx->sample_fmt = get_codec_sample_fmt(codec);
    audioCodecCtx->channel_layout = get_codec_channel_layout(codec);
    audioCodecCtx->sample_rate = AUDIO_RATE;
    audioCodecCtx->time_base = (AVRational) { 1, 1000 };
    audioCodecCtx->channels = av_get_channel_layout_nb_channels(audioCodecCtx->channel_layout);

    if (fmtCtx->oformat->flags & AVFMT_GLOBALHEADER)
        audioCodecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    int err;
    if ((err = avcodec_open2(audioCodecCtx, codec, NULL)) < 0)
    {
        std::cerr << "(audio) avcodec_open2 failed " << err << std::endl;
        std::exit(-1);
    }

    swrCtx = swr_alloc();
    if (!swrCtx)
    {
        std::cerr << "Faild to allocate swr context" << std::endl;
        std::exit(-1);
    }

    av_opt_set_int(swrCtx, "in_sample_rate", AUDIO_RATE, 0);
    av_opt_set_int(swrCtx, "out_sample_rate", audioCodecCtx->sample_rate, 0);
    av_opt_set_sample_fmt(swrCtx, "in_sample_fmt", AV_SAMPLE_FMT_FLT, 0);
    av_opt_set_sample_fmt(swrCtx, "out_sample_fmt", audioCodecCtx->sample_fmt, 0);
    av_opt_set_channel_layout(swrCtx, "in_channel_layout", AV_CH_LAYOUT_STEREO, 0);
    av_opt_set_channel_layout(swrCtx, "out_channel_layout", audioCodecCtx->channel_layout, 0);

    if (swr_init(swrCtx))
    {
        std::cerr << "Failed to initialize swr" << std::endl;
        std::exit(-1);
    }
}

void FrameWriter::init_codecs()
{
    init_video_stream();
    if (params.enable_audio)
        init_audio_stream();

    av_dump_format(fmtCtx, 0, params.file.c_str(), 1);
    if (avio_open(&fmtCtx->pb, params.file.c_str(), AVIO_FLAG_WRITE))
    {
        std::cerr << "avio_open failed" << std::endl;
        std::exit(-1);
    }

    AVDictionary *dummy = NULL;
    if (avformat_write_header(fmtCtx, &dummy) != 0)
    {
        std::cerr << "Failed to write file header" << std::endl;
        std::exit(-1);
    }
    av_dict_free(&dummy);
}

void FrameWriter::init_sws()
{
    switch (params.format)
    {
        case INPUT_FORMAT_BGR0:
            swsCtx = sws_getContext(params.width, params.height, AV_PIX_FMT_BGR0,
                params.width, params.height, PIX_FMT, SWS_FAST_BILINEAR, NULL, NULL, NULL);
            break;
        case INPUT_FORMAT_RGB0:
            swsCtx = sws_getContext(params.width, params.height, AV_PIX_FMT_RGB0,
                params.width, params.height, PIX_FMT, SWS_FAST_BILINEAR, NULL, NULL, NULL);
            break;
    }

    if (!swsCtx)
    {
        std::cerr << "Failed to create sws context" << std::endl;
        std::exit(-1);
    }
}

FrameWriter::FrameWriter(const FrameWriterParams& _params) :
    params(_params)
{
    if (params.enable_ffmpeg_debug_output)
        av_log_set_level(AV_LOG_DEBUG);

    // Preparing the data concerning the format and codec,
    // in order to write properly the header, frame data and end of file.
    this->outputFmt = av_guess_format(NULL, params.file.c_str(), NULL);
    if (!outputFmt)
    {
        std::cerr << "Failed to guess output format for file " << params.file << std::endl;
        std::exit(-1);
    }

    if (avformat_alloc_output_context2(&this->fmtCtx, NULL, NULL, params.file.c_str()) < 0)
    {
        std::cerr << "Failed to allocate output context" << std::endl;
        std::exit(-1);
    }

    init_codecs();

    // Allocating memory for each conversion output YUV frame.
    encoder_frame = av_frame_alloc();
    if (hw_device_context) {
        encoder_frame->format = params.format == INPUT_FORMAT_RGB0 ?
            AV_PIX_FMT_RGB0 : AV_PIX_FMT_BGR0;
    } else {
        encoder_frame->format = PIX_FMT;
    }
    encoder_frame->width = params.width;
    encoder_frame->height = params.height;
    if (av_frame_get_buffer(encoder_frame, 1))
    {
        std::cerr << "Failed to allocate frame buffer" << std::endl;
        std::exit(-1);
    }

    if (hw_device_context)
    {
        hw_frame = av_frame_alloc();
        AVHWFramesContext *frctx = (AVHWFramesContext*)hw_frame_context->data;
        hw_frame->format = frctx->format;
        hw_frame->hw_frames_ctx = av_buffer_ref(hw_frame_context);
        hw_frame->width = params.width;
        hw_frame->height = params.height;

        if (av_hwframe_get_buffer(hw_frame_context, hw_frame, 0))
        {
            std::cerr << "failed to hw frame buffer" << std::endl;
            std::exit(-1);
        }
    }
}

void FrameWriter::add_frame(const uint8_t* pixels, int64_t usec, bool y_invert)
{
    /* Calculate data after y-inversion */
    int stride[] = {int(4 * params.width)};
    const uint8_t *formatted_pixels = pixels;
    if (y_invert)
    {
        formatted_pixels += stride[0] * (params.height - 1);
        stride[0] *= -1;
    }

    AVFrame **output_frame;
    if (hw_device_context)
    {
        encoder_frame->data[0] = (uint8_t*)formatted_pixels;
        encoder_frame->linesize[0] = stride[0];

        if (av_hwframe_transfer_data(hw_frame, encoder_frame, 0))
        {
            std::cerr << "Failed to upload data to the gpu!" << std::endl;
            return;
        }

        output_frame = &hw_frame;
    } else
    {
        sws_scale(swsCtx, &formatted_pixels, stride, 0, params.height,
            encoder_frame->data, encoder_frame->linesize);

        output_frame = &encoder_frame;
    }

    (*output_frame)->pts = usec;

    AVPacket pkt;
    av_init_packet(&pkt);
    pkt.data = NULL;
    pkt.size = 0;

    int got_output;
    avcodec_encode_video2(videoCodecCtx, &pkt, *output_frame, &got_output);
    if (got_output)
      finish_frame(pkt, true);
}

#define SRC_RATE 1e6
#define DST_RATE 1e3

static int64_t conv_audio_pts(SwrContext *ctx, int64_t in)
{
    int64_t d = (int64_t) AUDIO_RATE * AUDIO_RATE;

    /* Convert from audio_src_tb to 1/(src_samplerate * dst_samplerate) */
    in = av_rescale_rnd(in, d, SRC_RATE, AV_ROUND_NEAR_INF);

    /* In units of 1/(src_samplerate * dst_samplerate) */
    in = swr_next_pts(ctx, in);

    /* Convert from 1/(src_samplerate * dst_samplerate) to audio_dst_tb */
    return av_rescale_rnd(in, DST_RATE, d, AV_ROUND_NEAR_INF);
}

void FrameWriter::send_audio_pkt(AVFrame *frame)
{
    AVPacket pkt;
    av_init_packet(&pkt);
    pkt.data = NULL;
    pkt.size = 0;

    int got_output;
    avcodec_encode_audio2(audioCodecCtx, &pkt, frame, &got_output);
    if (got_output)
      finish_frame(pkt, false);
}

size_t FrameWriter::get_audio_buffer_size()
{
    return audioCodecCtx->frame_size << 3;
}

void FrameWriter::add_audio(const void* buffer)
{
    AVFrame *inputf = av_frame_alloc();
    inputf->sample_rate    = AUDIO_RATE;
    inputf->format         = AV_SAMPLE_FMT_FLT;
    inputf->channel_layout = AV_CH_LAYOUT_STEREO;
    inputf->nb_samples     = audioCodecCtx->frame_size;

    av_frame_get_buffer(inputf, 0);
    memcpy(inputf->data[0], buffer, get_audio_buffer_size());

    AVFrame *outputf = av_frame_alloc();
    outputf->format         = audioCodecCtx->sample_fmt;
    outputf->sample_rate    = audioCodecCtx->sample_rate;
    outputf->channel_layout = audioCodecCtx->channel_layout;
    outputf->nb_samples     = audioCodecCtx->frame_size;
    av_frame_get_buffer(outputf, 0);

    outputf->pts = conv_audio_pts(swrCtx, INT64_MIN);
    swr_convert_frame(swrCtx, outputf, inputf);

    send_audio_pkt(outputf);

    av_frame_free(&inputf);
    av_frame_free(&outputf);
}

void FrameWriter::finish_frame(AVPacket& pkt, bool is_video)
{
    static std::mutex fmt_mutex, pending_mutex;

    if (is_video)
    {
        av_packet_rescale_ts(&pkt, (AVRational){ 1, 1000000 }, videoStream->time_base);
        pkt.stream_index = videoStream->index;
    } else
    {
        av_packet_rescale_ts(&pkt, (AVRational){ 1, 1000 }, audioStream->time_base);
        pkt.stream_index = audioStream->index;
    }

    /* We use two locks to ensure that if WLOG the audio thread is waiting for
     * the video one, when the video becomes ready the audio thread will be the
     * next one to obtain the lock */
    if (params.enable_audio)
    {
        pending_mutex.lock();
        fmt_mutex.lock();
        pending_mutex.unlock();
    }

    av_interleaved_write_frame(fmtCtx, &pkt);
    av_packet_unref(&pkt);

    if (params.enable_audio)
        fmt_mutex.unlock();
}

FrameWriter::~FrameWriter()
{
    // Writing the delayed frames:
    AVPacket pkt;
    av_init_packet(&pkt);

    for (int got_output = 1; got_output;)
    {
        avcodec_encode_video2(videoCodecCtx, &pkt, NULL, &got_output);
        if (got_output)
            finish_frame(pkt, true);
    }

    for (int got_output = 1; got_output && params.enable_audio;)
    {
        avcodec_encode_audio2(audioCodecCtx, &pkt, NULL, &got_output);
        if (got_output)
            finish_frame(pkt, false);
    }

    // Writing the end of the file.
    av_write_trailer(fmtCtx);

    // Closing the file.
    if (!(outputFmt->flags & AVFMT_NOFILE))
        avio_closep(&fmtCtx->pb);

    avcodec_close(videoStream->codec);
    // Freeing all the allocated memory:
    sws_freeContext(swsCtx);

    av_frame_free(&encoder_frame);
    if (params.enable_audio)
        avcodec_close(audioStream->codec);

    // TODO: free all the hw accel
    avformat_free_context(fmtCtx);
}
