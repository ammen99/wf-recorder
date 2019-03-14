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
    #include <libavutil/opt.h>
}

enum InputFormat
{
     INPUT_FORMAT_BGR0,
     INPUT_FORMAT_RGB0
};

class FrameWriter
{
    const unsigned int width, height;

    SwsContext* swsCtx;
    AVOutputFormat* fmt;
    AVStream* stream;
    AVFormatContext* fc;
    AVCodecContext* c;
    AVPacket pkt;

    AVFrame *yuvpic;
    void finish_frame();

public :
    FrameWriter(const std::string& filename, const std::string& codec,
        int width, int height, InputFormat format);
    void add_frame(const uint8_t* pixels, int msec, bool y_invert);
    ~FrameWriter();
};

#endif // FRAME_WRITER
