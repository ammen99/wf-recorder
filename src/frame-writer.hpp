// Adapted from https://stackoverflow.com/questions/34511312/how-to-encode-a-video-from-several-images-generated-in-a-c-program-without-wri
// (Later) adapted from https://github.com/apc-llc/moviemaker-cpp

#ifndef FRAME_WRITER
#define FRAME_WRITER

#include <stdint.h>
#include <string>
#include <vector>
#include <map>
#include <atomic>
#include "config.h"

extern "C"
{
    #include <libswresample/swresample.h>
    #include <libavcodec/avcodec.h>
#ifdef HAVE_LIBAVDEVICE
    #include <libavdevice/avdevice.h>
#endif
    #include <libavutil/mathematics.h>
    #include <libavformat/avformat.h>
    #include <libavfilter/avfilter.h>
    #include <libavfilter/buffersink.h>
    #include <libavfilter/buffersrc.h>
    #include <libavutil/pixdesc.h>
    #include <libavutil/hwcontext.h>
    #include <libavutil/opt.h>
    #include <libavutil/hwcontext_drm.h>
}

#include "config.h"

enum InputFormat
{
    INPUT_FORMAT_BGR0,
    INPUT_FORMAT_RGB0,
    INPUT_FORMAT_BGR8,
    INPUT_FORMAT_RGB565,
    INPUT_FORMAT_BGR565,
    INPUT_FORMAT_X2RGB10,
    INPUT_FORMAT_X2BGR10,
    INPUT_FORMAT_RGBX64,
    INPUT_FORMAT_BGRX64,
    INPUT_FORMAT_RGBX64F,
    INPUT_FORMAT_DMABUF,
};

struct FrameWriterParams
{
    std::string file;
    int width;
    int height;
    int stride;

    InputFormat format;
    int drm_format;

    std::string video_filter = "null"; // dummy filter

    std::string codec;
    std::string audio_codec;
    std::string muxer;
    std::string pix_fmt;
    std::string sample_fmt;
    std::string hw_device; // used only if codec contains vaapi
    std::map<std::string, std::string> codec_options;
    std::map<std::string, std::string> audio_codec_options;
    int framerate = 0;
    int sample_rate;
    int buffrate = 0;
    
    int64_t audio_sync_offset;

    bool enable_mouse = true;
    bool enable_audio;
    bool enable_ffmpeg_debug_output;

    int bframes;

    std::atomic<bool>& write_aborted_flag;
    FrameWriterParams(std::atomic<bool>& flag): write_aborted_flag(flag) {}
};

class FrameWriter
{
    FrameWriterParams params;
    void load_codec_options(AVDictionary **dict);
    void load_audio_codec_options(AVDictionary **dict);

    const AVOutputFormat* outputFmt;
    AVStream* videoStream;
    AVCodecContext* videoCodecCtx;
    AVFormatContext* fmtCtx;

    AVFilterContext* videoFilterSourceCtx = NULL;
    AVFilterContext* videoFilterSinkCtx = NULL;
    AVFilterGraph* videoFilterGraph = NULL;

    AVBufferRef *hw_device_context = NULL;
    AVBufferRef *hw_frame_context = NULL;
    AVBufferRef *hw_frame_context_in = NULL;

    std::map<struct gbm_bo*, AVFrame*> mapped_frames;

    AVPixelFormat lookup_pixel_format(std::string pix_fmt);
    AVPixelFormat handle_buffersink_pix_fmt(const AVCodec *codec);
    AVPixelFormat get_input_format();
    void init_hw_accel();
    void init_codecs();
    void init_video_filters(const AVCodec *codec);
    void init_video_stream();

    void encode(AVCodecContext *enc_ctx, AVFrame *frame, AVPacket *pkt);

#ifdef HAVE_PULSE
    SwrContext *swrCtx;
    AVStream *audioStream;
    AVCodecContext *audioCodecCtx;
    void init_swr();
    void init_audio_stream();
    void send_audio_pkt(AVFrame *frame);
#endif
    void finish_frame(AVCodecContext *enc_ctx, AVPacket& pkt);
    bool push_frame(AVFrame *frame, int64_t usec);

  public:
    FrameWriter(const FrameWriterParams& params);
    bool add_frame(const uint8_t* pixels, int64_t usec, bool y_invert);
    bool add_frame(struct gbm_bo *bo, int64_t usec, bool y_invert);

#ifdef HAVE_PULSE
    /* Buffer must have size get_audio_buffer_size() */
    void add_audio(const void* buffer);
    size_t get_audio_buffer_size();

#endif

    ~FrameWriter();
};

#include <memory>
#include <mutex>
#include <atomic>

extern std::mutex frame_writer_mutex, frame_writer_pending_mutex;
extern std::unique_ptr<FrameWriter> frame_writer;
extern std::atomic<bool> exit_main_loop;

#endif // FRAME_WRITER
