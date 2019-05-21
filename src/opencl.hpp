#pragma once

#define CL_TARGET_OPENCL_VERSION 220

#include <CL/opencl.h>
#include "frame-writer.hpp"

class OpenCL
{
    cl_mem yuv_buffer, rgb_buffer;
    unsigned int argbSize, yuvSize, yuvStride, width, height;
    cl_kernel kernel;
    cl_context context;
    cl_command_queue command_queue;
    cl_program program;

    public:

    OpenCL(int width, int height);
    ~OpenCL();

    int
    do_frame(const uint8_t* pixels, uint32_t **local_yuv_buffer, AVFrame *encoder_frame, AVPixelFormat format, bool y_invert);
};

extern std::unique_ptr<OpenCL> opencl;