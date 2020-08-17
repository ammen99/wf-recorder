// Adapted from https://stackoverflow.com/questions/34511312/how-to-encode-a-video-from-several-images-generated-in-a-c-program-without-wri
// (Later) adapted from https://github.com/apc-llc/moviemaker-cpp
//
// Audio encoding - thanks to wlstream, a lot of the code/ideas are taken from there

#include <iostream>
#include "frame-writer.hpp"
#include <vector>
#include <queue>
#include <cstring>
#include <sstream>
#include "averr.h"


static const AVRational US_RATIONAL{1,1000000} ;

// av_register_all was deprecated in 58.9.100, removed in 59.0.100
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(59, 0, 100)
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
#endif

void FrameWriter::init_hw_accel()
{
    int ret = av_hwdevice_ctx_create(&this->hw_device_context,
        av_hwdevice_find_type_by_name("vaapi"), params.hw_device.c_str(), NULL, 0);

    if (ret != 0)
    {
        std::cerr << "Failed to create hw encoding device " << params.hw_device << ": " << averr(ret) << std::endl;
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
    switch (params.format) {
    case INPUT_FORMAT_BGR0:
        return AV_PIX_FMT_BGR0;
    case INPUT_FORMAT_RGB0:
        return AV_PIX_FMT_RGB0;
    case INPUT_FORMAT_BGR8:
        return AV_PIX_FMT_RGB24;
    case INPUT_FORMAT_RGB565:
        return AV_PIX_FMT_RGB565LE;
    case INPUT_FORMAT_BGR565:
        return AV_PIX_FMT_BGR565LE;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(56, 55, 100)
    case INPUT_FORMAT_X2RGB10:
        return AV_PIX_FMT_X2RGB10LE;
#endif
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 7, 100)
    case INPUT_FORMAT_X2BGR10:
        return AV_PIX_FMT_X2BGR10LE;
#endif
    default:
        std::cerr << "Unknown format: " << params.format << std::endl;
        std::exit(-1);
    }
}

AVPixelFormat FrameWriter::lookup_pixel_format(std::string pix_fmt)
{
    AVPixelFormat fmt = av_get_pix_fmt(pix_fmt.c_str());

    if (fmt != AV_PIX_FMT_NONE)
      return fmt;

    std::cerr << "Failed to find the pixel format: " << pix_fmt << std::endl;
    std::exit(-1);
}

AVPixelFormat FrameWriter::choose_sw_format(const AVCodec *codec)
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

void FrameWriter::init_video_filters(const AVCodec *codec)
{
    if (params.codec.find("vaapi") != std::string::npos) {
        if (params.video_filter == "null") {
            // Add `hwupload,scale_vaapi=format=nv12` by default
            // It is necessary for conversion to a proper format.
            params.video_filter = "hwupload,scale_vaapi=format=nv12";
        }
    }

    this->videoFilterGraph = avfilter_graph_alloc();
    av_opt_set(videoFilterGraph, "scale_sws_opts", "flags=fast_bilinear:src_range=1:dst_range=1", 0);

    const AVFilter* source = avfilter_get_by_name("buffer");
    const AVFilter* sink   = avfilter_get_by_name("buffersink");

    if (!source || !sink) {
        std::cerr << "filtering source or sink element not found\n";
        exit(-1);
    }

    // Build the configuration of the 'buffer' filter.
    // See: ffmpeg -h filter=buffer
    // See: https://ffmpeg.org/ffmpeg-filters.html#buffer
    std::stringstream buffer_filter_config;
    buffer_filter_config << "video_size=" << params.width << "x" << params.height;
    buffer_filter_config << ":pix_fmt=" << (int)this->get_input_format();
    buffer_filter_config << ":time_base=" << US_RATIONAL.num << "/" << US_RATIONAL.den;
    buffer_filter_config << ":pixel_aspect=1/1";

    int err = avfilter_graph_create_filter(&this->videoFilterSourceCtx, source,
        "Source", buffer_filter_config.str().c_str(), NULL, this->videoFilterGraph);
    if (err < 0) {
        std::cerr << "Cannot create video filter in: " << averr(err) << std::endl;;
        exit(-1);
    }

    err = avfilter_graph_create_filter(&this->videoFilterSinkCtx, sink, "Sink",
        NULL, NULL, this->videoFilterGraph);
    if (err < 0) {
        std::cerr << "Cannot create video filter out: " << averr(err) << std::endl;;
        exit(-1);
    }

    // We also need to tell the sink which pixel formats are supported.
    // by the video encoder. codevIndicate to our sink  pixel formats
    // are accepted by our codec.
    const AVPixelFormat *supported_pix_fmts = codec->pix_fmts;
    static const AVPixelFormat only_yuv420p[] =
    {
        AV_PIX_FMT_YUV420P,
        AV_PIX_FMT_NONE
    };

    // Force pixel format to yuv420p if requested and possible
    if (params.force_yuv) {
        if (!supported_pix_fmts ||
            is_fmt_supported(AV_PIX_FMT_YUV420P, supported_pix_fmts)) {
            supported_pix_fmts = only_yuv420p ;
        } else {
            std::cerr << "Ignoring request to force yuv420p, " <<
                "because it is not supported by the codec\n";
        }
    }

    err = av_opt_set_int_list(this->videoFilterSinkCtx, "pix_fmts",
        supported_pix_fmts, AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);

    if (err < 0) {
        std::cerr << "Failed to set pix_fmts: " << averr(err) << std::endl;;
        exit(-1);
    }

    // Create the connections to the filter graph
    //
    // The in/out swap is not a mistake:
    //
    //   ----------       -----------------------------      --------
    //   | Source | ----> | in -> filter_graph -> out | ---> | Sink |
    //   ----------       -----------------------------      --------
    //
    // The 'in' of filter_graph is the output of the Source buffer
    // The 'out' of filter_graph is the input of the Sink buffer
    //

    AVFilterInOut *outputs = avfilter_inout_alloc();
    outputs->name       = av_strdup("in");
    outputs->filter_ctx = this->videoFilterSourceCtx;
    outputs->pad_idx    = 0;
    outputs->next       = NULL;

    AVFilterInOut *inputs  = avfilter_inout_alloc();
    inputs->name       = av_strdup("out");
    inputs->filter_ctx = this->videoFilterSinkCtx;
    inputs->pad_idx    = 0;
    inputs->next       = NULL;

    if (!outputs->name || !inputs->name) {
        std::cerr << "Failed to parse allocate inout filter links" << std::endl;
        exit(-1);
    }

    std::cout << "Using video filter: " << params.video_filter << std::endl;

    err = avfilter_graph_parse_ptr(this->videoFilterGraph,
        params.video_filter.c_str(), &inputs, &outputs, NULL);
    if (err < 0) {
        std::cerr << "Failed to parse graph filter: " << averr(err) << std::endl;;
        exit(-1) ;
    }

    // Filters that create HW frames ('hwupload', 'hwmap', ...) need
    // AVBufferRef in their hw_device_ctx. Unfortunately, there is no
    // simple API to do that for filters created by avfilter_graph_parse_ptr().
    // The code below is inspired from ffmpeg_filter.c
    if (this->hw_device_context) {
        for (unsigned i=0; i< this->videoFilterGraph->nb_filters; i++) {
            this->videoFilterGraph->filters[i]->hw_device_ctx =
                av_buffer_ref(this->hw_device_context);
        }
    }

    err = avfilter_graph_config(this->videoFilterGraph, NULL);
    if (err<0) {
        std::cerr << "Failed to configure graph filter: " << averr(err) << std::endl;;
        exit(-1) ;
    }

    if (params.enable_ffmpeg_debug_output) {
        std::cout << std::string(80,'#') << std::endl ;
        std::cout << avfilter_graph_dump(this->videoFilterGraph,0) << "\n";
        std::cout << std::string(80,'#') << std::endl ;
    }


    // The (input of the) sink is the output of the whole filter.
    AVFilterLink * filter_output = this->videoFilterSinkCtx->inputs[0] ;

    this->videoCodecCtx->width  = filter_output->w;
    this->videoCodecCtx->height = filter_output->h;
    this->videoCodecCtx->pix_fmt = (AVPixelFormat)filter_output->format;
    this->videoCodecCtx->time_base = filter_output->time_base;
    this->videoCodecCtx->framerate = filter_output->frame_rate; // can be 1/0 if unknown
    this->videoCodecCtx->sample_aspect_ratio = filter_output->sample_aspect_ratio;

    this->hw_frame_context = av_buffersink_get_hw_frames_ctx(
        this->videoFilterSinkCtx);
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
}

void FrameWriter::init_video_stream()
{
    AVDictionary *options = NULL;
    load_codec_options(&options);

    const AVCodec* codec = avcodec_find_encoder_by_name(params.codec.c_str());
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

    videoCodecCtx = avcodec_alloc_context3(codec);
    videoCodecCtx->width      = params.width;
    videoCodecCtx->height     = params.height;
    videoCodecCtx->time_base  = US_RATIONAL;
    videoCodecCtx->color_range = AVCOL_RANGE_JPEG;
    std::cout << "Framerate: " << params.framerate << std::endl;

    if (params.bframes != -1)
        videoCodecCtx->max_b_frames = params.bframes;

    if (!params.hw_device.empty()) {
        init_hw_accel();
    }

    // The filters need to be initialized after we have initialized
    // videoCodecCtx.
    //
    // After loading the filters, we should update the hw frames ctx.
    init_video_filters(codec);

    if (this->hw_frame_context) {
      videoCodecCtx->hw_frames_ctx = av_buffer_ref(this->hw_frame_context);
    }

    if (fmtCtx->oformat->flags & AVFMT_GLOBALHEADER) {
        videoCodecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    int ret;
    char err[256];
    if ((ret = avcodec_open2(videoCodecCtx, codec, &options)) < 0)
    {
        av_strerror(ret, err, 256);
        std::cerr << "avcodec_open2 failed: " << err << std::endl;
        std::exit(-1);
    }
    av_dict_free(&options);

    if ((ret = avcodec_parameters_from_context(videoStream->codecpar, videoCodecCtx)) < 0) {
        av_strerror(ret, err, 256);
        std::cerr << "avcodec_parameters_from_context failed: " << err << std::endl;
        std::exit(-1);
    }
}

#ifdef HAVE_PULSE
static uint64_t get_codec_channel_layout(const AVCodec *codec)
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

static enum AVSampleFormat get_codec_auto_sample_fmt(const AVCodec *codec)
{
    int i = 0;
    if (!codec->sample_fmts)
        return av_get_sample_fmt(DEFAULT_SAMPLE_FMT);
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
    } else if (check_fmt_available(codec, converted_fmt))
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

    audioCodecCtx = avcodec_alloc_context3(codec);
    if (params.sample_fmt.size() == 0) {
        audioCodecCtx->sample_fmt = get_codec_auto_sample_fmt(codec);
    } else {
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

    int ret;
    if ((ret = avcodec_parameters_from_context(audioStream->codecpar, audioCodecCtx)) < 0) {
        char errmsg[256];
        av_strerror(ret, errmsg, sizeof(errmsg));
        std::cerr << "avcodec_parameters_from_context failed: " << err << std::endl;
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

    init_codecs();
}

void FrameWriter::encode(AVCodecContext *enc_ctx, AVFrame *frame, AVPacket *pkt)
{
    /* send the frame to the encoder */
    int ret = avcodec_send_frame(enc_ctx, frame);
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

AVFrame *FrameWriter::prepare_frame_data(const uint8_t* pixels, bool y_invert)
{
    /* Calculate data after y-inversion */
    int stride[] = {int(params.stride)};
    const uint8_t *formatted_pixels = pixels;
    if (y_invert)
    {
        formatted_pixels += stride[0] * (params.height - 1);
        stride[0] *= -1;
    }

    auto frame = av_frame_alloc();
    if (!frame) {
        std::cerr << "Failed to allocate frame!" << std::endl;
        return NULL;
    }

    frame->data[0] = (uint8_t*)formatted_pixels;
    frame->linesize[0] = stride[0];
    frame->format = get_input_format();
    frame->width = params.width;
    frame->height = params.height;

    return frame;
}

bool FrameWriter::add_frame(const uint8_t* pixels, int64_t usec, bool y_invert)
{
    auto frame = prepare_frame_data(pixels, y_invert);
    frame->pts = usec; // We use time_base = 1/US_RATE

    // Push the RGB frame into the filtergraph */
    int err = av_buffersrc_add_frame_flags(videoFilterSourceCtx, frame, 0);
    if (err < 0) {
        std::cerr << "Error while feeding the filtergraph!" << std::endl;
        return false;
    }

    // Pull filtered frames from the filtergraph
    while (true) {
        AVFrame *filtered_frame = av_frame_alloc();

        if (!filtered_frame) {
            std::cerr << "Error av_frame_alloc" << std::endl;
            return false;
        }

        err = av_buffersink_get_frame(videoFilterSinkCtx, filtered_frame);
        if (err == AVERROR(EAGAIN)) {
            // Not an error. No frame available.
            // Try again later.
            break;
        } else if (err == AVERROR_EOF) {
            // There will be no more output frames on this sink.
            // That could happen if a filter like 'trim' is used to
            // stop after a given time.
            return false;
        } else if (err < 0) {
            av_frame_free(&filtered_frame);
            return false;
        }

        filtered_frame->pict_type = AV_PICTURE_TYPE_NONE;

        // So we have a frame. Encode it!
        AVPacket pkt;
        av_init_packet(&pkt);
        pkt.data = NULL;
        pkt.size = 0;

        encode(videoCodecCtx, filtered_frame, &pkt);
        av_frame_free(&filtered_frame);
    }

    av_frame_free(&frame);
    return true;
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
        av_packet_rescale_ts(&pkt, videoCodecCtx->time_base, videoStream->time_base);
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

    avcodec_free_context(&videoCodecCtx);
    // Freeing all the allocated memory:
#ifdef HAVE_PULSE
    if (params.enable_audio)
        avcodec_free_context(&audioCodecCtx);
#endif
    // TODO: free all the hw accel
    avformat_free_context(fmtCtx);
}
