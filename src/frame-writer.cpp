// Adapted from https://stackoverflow.com/questions/34511312/how-to-encode-a-video-from-several-images-generated-in-a-c-program-without-wri
// (Later) adapted from https://github.com/apc-llc/moviemaker-cpp
//
// Audio encoding - thanks to wlstream, a lot of the code/ideas are taken from there

#include <iostream>
#include "frame-writer.hpp"
#include <vector>
#include <queue>
#include <cstring>
#include "averr.h"

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
        std::cerr << "Failed to create hw encoding device " << params.hw_device << ": " << averr(ret) << std::endl;
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

    if ((ret = av_hwframe_ctx_init(hw_frame_context)))
    {
        std::cerr << "Failed to initialize hwframe context: " << averr(ret) << std::endl;
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

    if (params.codec.find("libx264") != std::string::npos ||
        params.codec.find("libx265") != std::string::npos)
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

void FrameWriter::load_acodec_options(AVDictionary **dict)
{
    for (auto& opt : params.acodec_options)
    {
        std::cout << "Setting codec option: " << opt.first << "=" << opt.second << std::endl;
        av_dict_set(dict, opt.first.c_str(), opt.second.c_str(), 0);
    }
}

bool is_fmt_supported(AVPixelFormat fmt, const AVPixelFormat *supported)
{
    for (int i = 0; supported[i] != AV_PIX_FMT_NONE; i++)
    {
        if (supported[i] == fmt)
            return true;
    }

    return false;
}

AVPixelFormat FrameWriter::get_input_format()
{
    return params.format == INPUT_FORMAT_BGR0 ?
        AV_PIX_FMT_BGR0 : AV_PIX_FMT_RGB0;
}

AVPixelFormat FrameWriter::lookup_pixel_format(std::string pix_fmt)
{
    AVPixelFormat fmt = av_get_pix_fmt(pix_fmt.c_str());

    if (fmt != AV_PIX_FMT_NONE)
      return fmt;

    std::cerr << "Failed to find the pixel format: " << pix_fmt << std::endl;
    std::exit(-1);
}

AVPixelFormat FrameWriter::choose_sw_format(AVCodec *codec)
{
    auto in_fmt = get_input_format();

    if (!params.pix_fmt.empty())
        return lookup_pixel_format(params.pix_fmt);

    /* For codecs such as rawvideo no supported formats are listed */
    if (!codec->pix_fmts)
        return in_fmt;

    /* If the codec supports getting the appropriate RGB format
     * directly, we want to use it since we don't have to convert data */
    if (is_fmt_supported(in_fmt, codec->pix_fmts))
        return in_fmt;

    /* Otherwise, try to use the already tested YUV420p */
    if (is_fmt_supported(AV_PIX_FMT_YUV420P, codec->pix_fmts))
        return AV_PIX_FMT_YUV420P;

    /* Lastly, use the first supported format */
    return codec->pix_fmts[0];
}

void FrameWriter::init_video_stream()
{
    AVDictionary *options = NULL;
    load_codec_options(&options);

    AVCodec* codec = avcodec_find_encoder_by_name(params.codec.c_str());
    if (!codec)
    {
        std::cerr << "Failed to find the given codec: " << params.codec << std::endl;
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
    std::cout << "Framerate: " << params.framerate << std::endl;
    videoCodecCtx->time_base = (AVRational){ 1, params.framerate };

    if (params.bframes != -1)
        videoCodecCtx->max_b_frames = params.bframes;

    if (params.codec.find("vaapi") != std::string::npos)
    {
        videoCodecCtx->pix_fmt = AV_PIX_FMT_VAAPI;
        init_hw_accel();
        videoCodecCtx->hw_frames_ctx = av_buffer_ref(hw_frame_context);

        if (params.force_yuv)
            init_sws(AV_PIX_FMT_YUV420P);
    } else
    {
        videoCodecCtx->pix_fmt = choose_sw_format(codec);
        std::cout << "Choosing pixel format " <<
            av_get_pix_fmt_name(videoCodecCtx->pix_fmt) << std::endl;
        init_sws(videoCodecCtx->pix_fmt);
    }

    if (fmtCtx->oformat->flags & AVFMT_GLOBALHEADER)
        videoCodecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    int ret;
    char err[256];
    if ((ret = avcodec_open2(videoCodecCtx, codec, &options)) < 0)
    {
        av_strerror(ret, err, 256);
        std::cerr << "avcodec_open2 failed: " << err << std::endl;
        std::exit(-1);
    }
    av_dict_free(&options);
}
#ifdef HAVE_PULSE
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

static enum AVSampleFormat get_codec_auto_sample_fmt(AVCodec *codec)
{
    int i = 0;
    if (!codec->sample_fmts)
        return av_get_sample_fmt(FALLBACK_SAMPLE_FMT);
    while (1) {
        if (codec->sample_fmts[i] == -1)
            break;
        if (av_get_bytes_per_sample(codec->sample_fmts[i]) >= 2)
            return codec->sample_fmts[i];
        i++;
    }
    return codec->sample_fmts[0];
}

bool check_fmt_available(AVCodec *codec, AVSampleFormat fmt){
    for (const enum AVSampleFormat *sample_ptr = codec -> sample_fmts; *sample_ptr != -1; sample_ptr++)
    {
        if (*sample_ptr == fmt)
        {
            return true;
        }
    }
    return false;
}

static enum AVSampleFormat convert_codec_sample_fmt(AVCodec *codec, std::string requested_fmt)
{
    static enum AVSampleFormat converted_fmt = av_get_sample_fmt(requested_fmt.c_str());
    if (converted_fmt == AV_SAMPLE_FMT_NONE)
    {
	std::cout << "Failed to find the given sample format: " << requested_fmt << std::endl;
	std::exit(-1);
    } else if (!codec->sample_fmts || check_fmt_available(codec, converted_fmt))
    {
        std::cout << "Using sample format " << av_get_sample_fmt_name(converted_fmt) << std::endl;
        return converted_fmt;
    } else
    {
	std::cout << "Codec " << codec->name << " does not support sample format " << av_get_sample_fmt_name(converted_fmt) << std::endl;
	std::exit(-1);
    }
}

void FrameWriter::init_audio_stream()
{
    AVDictionary *options = NULL;
    load_codec_options(&options);
    
    AVCodec* codec = avcodec_find_encoder_by_name(params.acodec.c_str());
    if (!codec)
    {
        std::cerr << "Failed to find the given audio codec: " << params.acodec << std::endl;
        std::exit(-1);
    }

    audioStream = avformat_new_stream(fmtCtx, codec);
    if (!audioStream)
    {
        std::cerr << "Failed to open audio stream" << std::endl;
        std::exit(-1);
    }

    audioCodecCtx = audioStream->codec;
    if (params.codec == "aac"){
        audioCodecCtx->bit_rate = lrintf(128000.0f);
    }
    if (params.sample_fmt.size() == 0){
	audioCodecCtx->sample_fmt = get_codec_auto_sample_fmt(codec);
	std::cout << "Choosing sample format " << av_get_sample_fmt_name(audioCodecCtx->sample_fmt) << " for audio codec " << codec->name << std::endl;
    } else
    {
	audioCodecCtx->sample_fmt = convert_codec_sample_fmt(codec, params.sample_fmt);
    }
    audioCodecCtx->channel_layout = get_codec_channel_layout(codec);
    audioCodecCtx->sample_rate = params.sample_rate;
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
        std::cerr << "Failed to allocate swr context" << std::endl;
        std::exit(-1);
    }

    av_opt_set_int(swrCtx, "in_sample_rate", params.sample_rate, 0);
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
#endif
void FrameWriter::init_codecs()
{
    init_video_stream();
#ifdef HAVE_PULSE
    if (params.enable_audio)
        init_audio_stream();
#endif
    av_dump_format(fmtCtx, 0, params.file.c_str(), 1);
    if (avio_open(&fmtCtx->pb, params.file.c_str(), AVIO_FLAG_WRITE))
    {
        std::cerr << "avio_open failed" << std::endl;
        std::exit(-1);
    }

    AVDictionary *dummy = NULL;
    char err[256];
    int ret;
    if ((ret = avformat_write_header(fmtCtx, &dummy)) != 0)
    {
        std::cerr << "Failed to write file header" << std::endl;
        av_strerror(ret, err, 256);
        std::cerr << err << std::endl;
        std::exit(-1);
    }
    av_dict_free(&dummy);
}

void FrameWriter::init_sws(AVPixelFormat format)
{
    swsCtx = sws_getContext(params.width, params.height, get_input_format(),
        params.width, params.height, format,
        SWS_FAST_BILINEAR, NULL, NULL, NULL);

    if (!swsCtx)
    {
        std::cerr << "Failed to create sws context" << std::endl;
        std::exit(-1);
    }
}

static const char* determine_output_format(const FrameWriterParams& params)
{
    if (!params.muxer.empty())
        return params.muxer.c_str();

    if (params.file.find("rtmp") == 0)
        return "flv";

    if (params.file.find("udp") == 0)
        return "mpegts";

    return NULL;
}

FrameWriter::FrameWriter(const FrameWriterParams& _params) :
    params(_params)
{
    if (params.enable_ffmpeg_debug_output)
        av_log_set_level(AV_LOG_DEBUG);

#ifdef HAVE_LIBAVDEVICE
    avdevice_register_all();
#endif

    // Preparing the data concerning the format and codec,
    // in order to write properly the header, frame data and end of file.
    this->outputFmt = av_guess_format(NULL, params.file.c_str(), NULL);
    auto streamFormat = determine_output_format(params);
    auto context_ret = avformat_alloc_output_context2(&this->fmtCtx, NULL,
        streamFormat, params.file.c_str());
    if (context_ret < 0)
    {
        std::cerr << "Failed to allocate output context" << std::endl;
        std::exit(-1);
    }

#ifndef HAVE_OPENCL
    if (params.opencl)
        std::cerr << "This version of wf-recorder was built without OpenCL support. Ignoring OpenCL option." << std::endl;
#endif

    init_codecs();

    encoder_frame = av_frame_alloc();
    if (hw_device_context) {
        encoder_frame->format = params.force_yuv ? AV_PIX_FMT_YUV420P : get_input_format();
    } else {
        encoder_frame->format = videoCodecCtx->pix_fmt;
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

void FrameWriter::convert_pixels_to_yuv(const uint8_t *pixels,
    const uint8_t *formatted_pixels, int stride[])
{
    bool y_invert = (pixels != formatted_pixels);
    bool converted_with_opencl = false;

#ifdef HAVE_OPENCL
    if (params.opencl && params.force_yuv)
    {
        int r = opencl->do_frame(pixels, encoder_frame,
            get_input_format(), y_invert);

        converted_with_opencl = (r == 0);
    }
#else
    /* Silence compiler warning when opencl is disabled */
    (void)(y_invert);
#endif

    if (!converted_with_opencl)
    {
        sws_scale(swsCtx, &formatted_pixels, stride, 0, params.height,
            encoder_frame->data, encoder_frame->linesize);
    }
}

void FrameWriter::encode(AVCodecContext *enc_ctx, AVFrame *frame, AVPacket *pkt)
{
    int ret;

    /* send the frame to the encoder */
    ret = avcodec_send_frame(enc_ctx, frame);
    if (ret < 0)
    {
        fprintf(stderr, "error sending a frame for encoding\n");
        return;
    }

    while (ret >= 0)
    {
        ret = avcodec_receive_packet(enc_ctx, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        {
            return;
        }
        if (ret < 0)
        {
            fprintf(stderr, "error during encoding\n");
            return;
        }

        finish_frame(enc_ctx, *pkt);
    }
}

void FrameWriter::add_frame(const uint8_t* pixels, int64_t usec, bool y_invert)
{
    /* Calculate data after y-inversion */
    int stride[] = {int(params.stride)};
    const uint8_t *formatted_pixels = pixels;
    if (y_invert)
    {
        formatted_pixels += stride[0] * (params.height - 1);
        stride[0] *= -1;
    }

    AVFrame **output_frame;
    AVBufferRef *saved_buf0 = NULL;

    if (hw_device_context)
    {
        if (params.force_yuv) {
            convert_pixels_to_yuv(pixels, formatted_pixels, stride);
        } else {
            encoder_frame->data[0] = (uint8_t*) formatted_pixels;
            encoder_frame->linesize[0] = stride[0];
        }

        if (av_hwframe_transfer_data(hw_frame, encoder_frame, 0))
        {
            std::cerr << "Failed to upload data to the gpu!" << std::endl;
            return;
        }

        output_frame = &hw_frame;
    } else if(get_input_format() == videoCodecCtx->pix_fmt)
    {
        output_frame = &encoder_frame;
        encoder_frame->data[0] = (uint8_t*)formatted_pixels;
        encoder_frame->linesize[0] = stride[0];
        /* Force ffmpeg to create a copy of the frame, if the codec needs it */
        saved_buf0 = encoder_frame->buf[0];
        encoder_frame->buf[0] = NULL;
    } else
    {
        convert_pixels_to_yuv(pixels, formatted_pixels, stride);

        /* Force ffmpeg to create a copy of the frame, if the codec needs it */
        saved_buf0 = encoder_frame->buf[0];
        encoder_frame->buf[0] = NULL;
        output_frame = &encoder_frame;
    }

    (*output_frame)->pts = usec;

    AVPacket pkt;
    av_init_packet(&pkt);
    pkt.data = NULL;
    pkt.size = 0;

    encode(videoCodecCtx, *output_frame, &pkt);

    /* Restore frame buffer, so that it can be properly freed in the end */
    if (saved_buf0)
        encoder_frame->buf[0] = saved_buf0;
}
#ifdef HAVE_PULSE
#define SRC_RATE 1e6
#define DST_RATE 1e3

static int64_t conv_audio_pts(SwrContext *ctx, int64_t in, int sample_rate)
{
    //int64_t d = (int64_t) AUDIO_RATE * AUDIO_RATE;
    int64_t d = (int64_t) sample_rate * sample_rate;

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

    encode(audioCodecCtx, frame, &pkt);
}

size_t FrameWriter::get_audio_buffer_size()
{
    return audioCodecCtx->frame_size << 3;
}

void FrameWriter::add_audio(const void* buffer)
{
    AVFrame *inputf = av_frame_alloc();
    //inputf->sample_rate    = AUDIO_RATE;
    inputf->sample_rate    = params.sample_rate;
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

    outputf->pts = conv_audio_pts(swrCtx, INT64_MIN, params.sample_rate);
    swr_convert_frame(swrCtx, outputf, inputf);

    send_audio_pkt(outputf);

    av_frame_free(&inputf);
    av_frame_free(&outputf);
}
#endif
void FrameWriter::finish_frame(AVCodecContext *enc_ctx, AVPacket& pkt)
{
    static std::mutex fmt_mutex, pending_mutex;

    if (enc_ctx == videoCodecCtx)
    {
        av_packet_rescale_ts(&pkt, (AVRational){ 1, 1000000 }, videoStream->time_base);
        pkt.stream_index = videoStream->index;
    }
#ifdef HAVE_PULSE
    else
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
#endif
    if (av_interleaved_write_frame(fmtCtx, &pkt) != 0) {
        params.write_aborted_flag = true;
    }
    av_packet_unref(&pkt);
#ifdef HAVE_PULSE
    if (params.enable_audio)
        fmt_mutex.unlock();
#endif
}

FrameWriter::~FrameWriter()
{
    // Writing the delayed frames:
    AVPacket pkt;
    av_init_packet(&pkt);

    encode(videoCodecCtx, NULL, &pkt);
#ifdef HAVE_PULSE
    if (params.enable_audio)
    {
        encode(audioCodecCtx, NULL, &pkt);
    }
#endif
    // Writing the end of the file.
    av_write_trailer(fmtCtx);

    // Closing the file.
    if (outputFmt && (!(outputFmt->flags & AVFMT_NOFILE)))
        avio_closep(&fmtCtx->pb);

    avcodec_close(videoStream->codec);
    // Freeing all the allocated memory:
    sws_freeContext(swsCtx);

    av_frame_free(&encoder_frame);
#ifdef HAVE_PULSE
    if (params.enable_audio)
        avcodec_close(audioStream->codec);
#endif
    // TODO: free all the hw accel
    avformat_free_context(fmtCtx);
}
