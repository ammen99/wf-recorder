/* Copyright 2019 Scott Moreau */

#pragma once

#define CL_TARGET_OPENCL_VERSION 120

#include <CL/opencl.h>
#include "frame-writer.hpp"

class OpenCL
{
    cl_mem yuv420_buffer, rgb_buffer;
    unsigned int argbSize, yuv420Size, width, height, halfWidth, halfHeight;
    cl_kernel kernel;
    cl_context context;
    cl_command_queue command_queue;
    cl_program program;
    cl_int ret = 0;
    uint8_t *local_yuv420_buffer;

    public:

    OpenCL(int width, int height);
    ~OpenCL();

    int
    do_frame(const uint8_t* pixels, AVFrame *encoder_frame, AVPixelFormat format, bool y_invert);
};

extern std::unique_ptr<OpenCL> opencl;