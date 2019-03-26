// Adapted from https://stackoverflow.com/questions/34511312/how-to-encode-a-video-from-several-images-generated-in-a-c-program-without-wri
// (Later) adapted from https://github.com/apc-llc/moviemaker-cpp

#ifndef FRAME_WRITER
#define FRAME_WRITER

#include <stdint.h>
#include <string>
#include <vector>
#include <map>

extern "C"
{
    #include <x264.h>
    #include <libswscale/swscale.h>
    #include <libavcodec/avcodec.h>
    #include <libavutil/mathematics.h>
    #include <libavformat/avformat.h>
    #include <libavutil/hwcontext.h>
    #include <libavutil/opt.h>
}

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

    bool enable_ffmpeg_debug_output;
};

class FrameWriter
{
    FrameWriterParams params;
    void load_codec_options(AVDictionary **dict);

    SwsContext* swsCtx;
    AVOutputFormat* outputFmt;
    AVStream* stream;
    AVFormatContext* fmtCtx;
    AVCodecContext* codecCtx;
    AVPacket pkt;

    AVBufferRef *hw_device_context = NULL;
    AVBufferRef *hw_frame_context = NULL;

    InputFormat *input_format;
    void init_hw_accel();
    void init_sws();
    void init_codec();

    AVFrame *encoder_frame = NULL;
    AVFrame *hw_frame = NULL;

    void finish_frame();

public :
    FrameWriter(const FrameWriterParams& params);
    void add_frame(const uint8_t* pixels, int msec, bool y_invert);
    ~FrameWriter();
};

#endif // FRAME_WRITER
