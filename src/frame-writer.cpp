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
#include <gbm.h>


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
    using CodecOptions = std::map<std::string, std::string>;

    static const CodecOptions default_x264_options = {
        {"tune", "zerolatency"},
        {"preset", "ultrafast"},
        {"crf", "20"},
    };

    static const CodecOptions default_libvpx_options = {
        {"cpu-used", "5"},
        {"deadline", "realtime"},
    };

    static const std::map<std::string, const CodecOptions&> default_codec_options = {
        {"libx264", default_x264_options},
        {"libx265", default_x264_options},
        {"libvpx", default_libvpx_options},
    };

    for (const auto& opts : default_codec_options)
    {
        if (params.codec.find(opts.first) != std::string::npos)
        {
            for (const auto& param : opts.second)
            {
                if (!params.codec_options.count(param.first))
                    params.codec_options[param.first] = param.second;
            }
            break;
        }
    }

    for (auto& opt : params.codec_options)
    {
        std::cout << "Setting codec option: " << opt.first << "=" << opt.second << std::endl;
        av_dict_set(dict, opt.first.c_str(), opt.second.c_str(), 0);
    }
}

void FrameWriter::load_audio_codec_options(AVDictionary **dict)
{
    for (auto& opt : params.audio_codec_options)
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
    case INPUT_FORMAT_RGBX64:
        return AV_PIX_FMT_RGBA64LE;
    case INPUT_FORMAT_BGRX64:
        return AV_PIX_FMT_BGRA64LE;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 33, 101)
    case INPUT_FORMAT_RGBX64F:
        return AV_PIX_FMT_RGBAF16LE;
#endif
    case INPUT_FORMAT_DMABUF:
        return AV_PIX_FMT_VAAPI;
    default:
        std::cerr << "Unknown format: " << params.format << std::endl;
        std::exit(-1);
    }
}

static const struct {
    int drm;
    AVPixelFormat av;
} drm_av_format_table [] = {
    { GBM_FORMAT_ARGB8888, AV_PIX_FMT_BGRA },
    { GBM_FORMAT_XRGB8888, AV_PIX_FMT_BGR0 },
    { GBM_FORMAT_ABGR8888, AV_PIX_FMT_RGBA },
    { GBM_FORMAT_XBGR8888, AV_PIX_FMT_RGB0 },
    { GBM_FORMAT_RGBA8888, AV_PIX_FMT_ABGR },
    { GBM_FORMAT_RGBX8888, AV_PIX_FMT_0BGR },
    { GBM_FORMAT_BGRA8888, AV_PIX_FMT_ARGB },
    { GBM_FORMAT_BGRX8888, AV_PIX_FMT_0RGB },
};

static AVPixelFormat get_drm_av_format(int fmt)
{
    for (size_t i = 0; i < sizeof(drm_av_format_table) / sizeof(drm_av_format_table[0]); ++i) {
        if (drm_av_format_table[i].drm == fmt) {
            return drm_av_format_table[i].av;
        }
    }
    std::cerr << "Failed to find AV format for" << fmt;
    return AV_PIX_FMT_RGBA;
}

AVPixelFormat FrameWriter::lookup_pixel_format(std::string pix_fmt)
{
    AVPixelFormat fmt = av_get_pix_fmt(pix_fmt.c_str());

    if (fmt != AV_PIX_FMT_NONE)
      return fmt;

    std::cerr << "Failed to find the pixel format: " << pix_fmt << std::endl;
    std::exit(-1);
}

AVPixelFormat FrameWriter::handle_buffersink_pix_fmt(const AVCodec *codec)
{

    // Return with user chosen format
    if (!params.pix_fmt.empty())
        return lookup_pixel_format(params.pix_fmt);

    auto in_fmt = get_input_format();

    /* For codecs such as rawvideo no supported formats are listed */
    if (!codec->pix_fmts)
        return in_fmt;

    /* If the codec supports getting the appropriate RGB format
     * directly, we want to use it since we don't have to convert data */
    if (is_fmt_supported(in_fmt, codec->pix_fmts))
        return in_fmt;

    /* Choose the format supported by the codec which best approximates the
     * input fmt. */
    AVPixelFormat best_format = AV_PIX_FMT_NONE;
    for (int i = 0; codec->pix_fmts[i] != AV_PIX_FMT_NONE; i++) {
        int loss = 0;
        best_format = av_find_best_pix_fmt_of_2(best_format,
            codec->pix_fmts[i], in_fmt, false, &loss);
    }
    return best_format;
}

void FrameWriter::init_video_filters(const AVCodec *codec)
{
    if (params.framerate != 0){
        if (params.video_filter != "null" && params.video_filter.find("fps") == std::string::npos) {
            params.video_filter += ",fps=" + std::to_string(params.framerate);
        }
        else if (params.video_filter == "null"){
            params.video_filter = "fps=" + std::to_string(params.framerate);
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

    if (this->hw_device_context) {
        this->hw_frame_context = av_hwframe_ctx_alloc(this->hw_device_context);
        AVHWFramesContext *hwfc = reinterpret_cast<AVHWFramesContext*>(this->hw_frame_context->data);
        hwfc->format = AV_PIX_FMT_VAAPI;
        hwfc->sw_format = AV_PIX_FMT_NV12;
        hwfc->width = params.width;
        hwfc->height = params.height;
        int err = av_hwframe_ctx_init(this->hw_frame_context);
        if (err < 0) {
            std::cerr << "Cannot create hw frames context: " << averr(err) << std::endl;
            exit(-1);
        }

        this->hw_frame_context_in = av_hwframe_ctx_alloc(this->hw_device_context);
        hwfc = reinterpret_cast<AVHWFramesContext*>(this->hw_frame_context_in->data);
        hwfc->format = AV_PIX_FMT_VAAPI;
        hwfc->sw_format = get_drm_av_format(params.drm_format);
        hwfc->width = params.width;
        hwfc->height = params.height;
        err = av_hwframe_ctx_init(this->hw_frame_context_in);
        if (err < 0) {
            std::cerr << "Cannot create hw frames context: " << averr(err) << std::endl;
            exit(-1);
        }
    }

    // Build the configuration of the 'buffer' filter.
    // See: ffmpeg -h filter=buffer
    // See: https://ffmpeg.org/ffmpeg-filters.html#buffer
    std::stringstream buffer_filter_config;
    buffer_filter_config << "video_size=" << params.width << "x" << params.height;
    buffer_filter_config << ":pix_fmt=" << (int)this->get_input_format();
    buffer_filter_config << ":time_base=" << US_RATIONAL.num << "/" << US_RATIONAL.den;
    if (params.buffrate != 0) {
        buffer_filter_config << ":frame_rate=" << params.buffrate;
    }
    buffer_filter_config << ":pixel_aspect=1/1";

    int err = avfilter_graph_create_filter(&this->videoFilterSourceCtx, source,
        "Source", buffer_filter_config.str().c_str(), NULL, this->videoFilterGraph);
    if (err < 0) {
        std::cerr << "Cannot create video filter in: " << averr(err) << std::endl;;
        exit(-1);
    }

    AVBufferSrcParameters *p = av_buffersrc_parameters_alloc();
    memset(p, 0, sizeof(*p));
    p->format = AV_PIX_FMT_NONE;
    p->hw_frames_ctx = this->hw_frame_context_in;
    err = av_buffersrc_parameters_set(this->videoFilterSourceCtx, p);
    av_free(p);
    if (err < 0) {
         std::cerr << "Cannot set hwcontext filter in: " << averr(err) << std::endl;;
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
    const AVPixelFormat picked_pix_fmt[] =
    {
        handle_buffersink_pix_fmt(codec),
        AV_PIX_FMT_NONE
    };

    err = av_opt_set_int_list(this->videoFilterSinkCtx, "pix_fmts",
        picked_pix_fmt, AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);

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
    if (params.framerate) {
    std::cout << "Framerate: " << params.framerate << std::endl;
    }

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
        return av_get_sample_fmt(FALLBACK_AUDIO_SAMPLE_FMT);
    while (1) {
        if (codec->sample_fmts[i] == -1)
            break;
        if (av_get_bytes_per_sample(codec->sample_fmts[i]) >= 2)
            return codec->sample_fmts[i];
        i++;
    }
    return codec->sample_fmts[0];
}

bool check_fmt_available(const AVCodec *codec, AVSampleFormat fmt){
    for (const enum AVSampleFormat *sample_ptr = codec -> sample_fmts; *sample_ptr != -1; sample_ptr++)
    {
        if (*sample_ptr == fmt)
        {
            return true;
        }
    }
    return false;
}

static enum AVSampleFormat convert_codec_sample_fmt(const AVCodec *codec, std::string requested_fmt)
{
    static enum AVSampleFormat converted_fmt = av_get_sample_fmt(requested_fmt.c_str());
    if (converted_fmt == AV_SAMPLE_FMT_NONE)
    {
	std::cout << "Failed to find the given sample format: " << requested_fmt << std::endl;
	std::exit(-1);
    } else if (!codec->sample_fmts || check_fmt_available(codec, converted_fmt))
    {
        std::cout << "Using sample format " << av_get_sample_fmt_name(converted_fmt) << " for audio codec " << codec->name << std::endl;
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
    
    const AVCodec* codec = avcodec_find_encoder_by_name(params.audio_codec.c_str());
    if (!codec)
    {
        std::cerr << "Failed to find the given audio codec: " << params.audio_codec << std::endl;
        std::exit(-1);
    }

    audioStream = avformat_new_stream(fmtCtx, codec);
    if (!audioStream)
    {
        std::cerr << "Failed to open audio stream" << std::endl;
        std::exit(-1);
    }

    audioCodecCtx = avcodec_alloc_context3(codec);
    if (params.sample_fmt.size() == 0) 
    {
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

bool FrameWriter::push_frame(AVFrame *frame, int64_t usec)
{
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
            av_frame_free(&filtered_frame);
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
        AVPacket *pkt = av_packet_alloc();
        pkt->data = NULL;
        pkt->size = 0;

        encode(videoCodecCtx, filtered_frame, pkt);
        av_frame_free(&filtered_frame);
        av_packet_free(&pkt);
    }

    av_frame_free(&frame);
    return true;
}

bool FrameWriter::add_frame(const uint8_t* pixels, int64_t usec, bool y_invert)
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
        return false;
    }

    frame->data[0] = (uint8_t*)formatted_pixels;
    frame->linesize[0] = stride[0];
    frame->format = get_input_format();
    frame->width = params.width;
    frame->height = params.height;

    return push_frame(frame, usec);
}

bool FrameWriter::add_frame(struct gbm_bo *bo, int64_t usec, bool y_invert)
{
    if (y_invert)
    {
        std::cerr << "Y_INVERT not supported with dmabuf" << std::endl;
        return false;
    }

    auto frame = av_frame_alloc();
    if (!frame)
    {
        std::cerr << "Failed to allocate frame!" << std::endl;
        return false;
    }

    if (mapped_frames.find(bo) == mapped_frames.end()) {
        auto vaapi_frame = av_frame_alloc();
        if (!vaapi_frame) {
            std::cerr << "Failed to allocate frame!" << std::endl;
            return false;
        }

        AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor*) av_mallocz(sizeof(AVDRMFrameDescriptor));
        desc->nb_layers = 1;
        desc->nb_objects = 1;
        desc->objects[0].fd = gbm_bo_get_fd(bo);
        desc->objects[0].format_modifier = gbm_bo_get_modifier(bo);
        desc->objects[0].size = gbm_bo_get_stride(bo) * gbm_bo_get_height(bo);
        desc->layers[0].format = gbm_bo_get_format(bo);
        desc->layers[0].nb_planes = gbm_bo_get_plane_count(bo);
        for (int i = 0; i < gbm_bo_get_plane_count(bo); ++i) {
            desc->layers[0].planes[i].object_index = 0;
            desc->layers[0].planes[i].pitch = gbm_bo_get_stride_for_plane(bo, i);
            desc->layers[0].planes[i].offset = gbm_bo_get_offset(bo, i);
        }

        frame->width = gbm_bo_get_width(bo);
        frame->height = gbm_bo_get_height(bo);
        frame->format = AV_PIX_FMT_DRM_PRIME;
        frame->data[0] = reinterpret_cast<uint8_t*>(desc);
        frame->buf[0] = av_buffer_create(frame->data[0], sizeof(*desc),
            [](void *, uint8_t *data) {
                av_free(data);
        }, frame, 0);

        vaapi_frame->format = AV_PIX_FMT_VAAPI;
        vaapi_frame->hw_frames_ctx = av_buffer_ref(this->hw_frame_context_in);

        int ret = av_hwframe_map(vaapi_frame, frame, AV_HWFRAME_MAP_READ);
        av_frame_unref(frame);
        if (ret < 0)
        {
            std::cerr << "Failed to map vaapi frame " << averr(ret) << std::endl;
            return false;
        }

        mapped_frames[bo] = vaapi_frame;
    }

    av_frame_ref(frame, mapped_frames[bo]);
    return push_frame(frame, usec);
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
    AVPacket *pkt = av_packet_alloc();
    pkt->data = NULL;
    pkt->size = 0;

    encode(audioCodecCtx, frame, pkt);
    av_packet_free(&pkt);
}

size_t FrameWriter::get_audio_buffer_size()
{
    return audioCodecCtx->frame_size << 3;
}

void FrameWriter::add_audio(const void* buffer)
{
    AVFrame *inputf = av_frame_alloc();
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
    AVPacket *pkt = av_packet_alloc();

    if (videoCodecCtx->delay)
    {
        encode(videoCodecCtx, NULL, pkt);
#ifdef HAVE_PULSE
        if (params.enable_audio)
        {
            encode(audioCodecCtx, NULL, pkt);
        }
#endif
    }
    // Writing the end of the file.
    av_write_trailer(fmtCtx);

    // Closing the file.
    if (outputFmt && (!(outputFmt->flags & AVFMT_NOFILE)))
        avio_closep(&fmtCtx->pb);

    // Freeing all the allocated memory:
    avcodec_free_context(&videoCodecCtx);
#ifdef HAVE_PULSE
    if (params.enable_audio)
        avcodec_free_context(&audioCodecCtx);
#endif
    av_packet_free(&pkt);
    // TODO: free all the hw accel
    avformat_free_context(fmtCtx);
}
