// Adapted from https://stackoverflow.com/questions/34511312/how-to-encode-a-video-from-several-images-generated-in-a-c-program-without-wri
// (Later) adapted from https://github.com/apc-llc/moviemaker-cpp

#ifndef FRAME_WRITER
#define FRAME_WRITER

#include <stdint.h>
#include <string>
#include <vector>
#include <map>

#define AUDIO_RATE 44100

extern "C"
{
    #include <libswscale/swscale.h>
    #include <libswresample/swresample.h>
    #include <libavcodec/avcodec.h>
    #include <libavutil/mathematics.h>
    #include <libavformat/avformat.h>
    #include <libavutil/pixdesc.h>
    #include <libavutil/hwcontext.h>
    #include <libavutil/opt.h>
}

#include "config.h"

#ifdef HAVE_OPENCL
#include <memory>
#include "opencl.hpp"
class OpenCL;
#endif

enum InputFormat
{
     INPUT_FORMAT_BGR0,
     INPUT_FORMAT_RGB0
};

struct FrameWriterParams
{
    std::string file;
    int width;
    int height;

    InputFormat format;

    std::string codec;
    std::string hw_device; // used only if codec contains vaapi
    std::map<std::string, std::string> codec_options;

    int64_t audio_sync_offset;

    bool enable_audio;
    bool enable_ffmpeg_debug_output;

    bool opencl;
    bool force_yuv;
    int opencl_device;
};

class FrameWriter
{
    FrameWriterParams params;
    void load_codec_options(AVDictionary **dict);

    SwsContext* swsCtx;
    AVOutputFormat* outputFmt;
    AVStream* videoStream;
    AVCodecContext* videoCodecCtx;
    AVFormatContext* fmtCtx;

    AVBufferRef *hw_device_context = NULL;
    AVBufferRef *hw_frame_context = NULL;

    AVPixelFormat choose_sw_format(AVCodec *codec);
    AVPixelFormat get_input_format();
    void init_hw_accel();
    void init_sws(AVPixelFormat format);
    void init_codecs();
    void init_video_stream();

    AVFrame *encoder_frame = NULL;
    AVFrame *hw_frame = NULL;

    /**
     * Convert the given pixels to YUV and store in encoder_frame.
     * Calls OpenCL if it is enabled.
     *
     * @param formatted_pixels contains the same data as pixels but y-inverted
     * if the input format requires y-inversion.
     */
    void convert_pixels_to_yuv(const uint8_t *pixels,
        const uint8_t *formatted_pixels, int stride[]);

    void encode(AVCodecContext *enc_ctx, AVFrame *frame, AVPacket *pkt);

    SwrContext *swrCtx;
    AVStream *audioStream;
    AVCodecContext *audioCodecCtx;
    void init_swr();
    void init_audio_stream();
    void send_audio_pkt(AVFrame *frame);

    void finish_frame(AVPacket& pkt, bool isVideo);

public :
    FrameWriter(const FrameWriterParams& params);
    void add_frame(const uint8_t* pixels, int64_t usec, bool y_invert);

    /* Buffer must have size get_audio_buffer_size() */
    void add_audio(const void* buffer);
    size_t get_audio_buffer_size();

#ifdef HAVE_OPENCL
    std::unique_ptr<OpenCL> opencl;
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
