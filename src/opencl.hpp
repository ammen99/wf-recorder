/* Copyright 2019 Scott Moreau */

#pragma once

#define CL_TARGET_OPENCL_VERSION 110

#include <CL/opencl.h>
#include "frame-writer.hpp"

class OpenCL
{
    cl_device_id device_id;
    cl_mem yuv420_buffer, rgb_buffer;
    unsigned int argbSize, yuv420Size, width, height, halfWidth, halfHeight;
    cl_kernel kernel;
    cl_context context;
    cl_command_queue command_queue;
    cl_program program;
    cl_int ret = 0;
    uint8_t *local_yuv420_buffer;
    cl_device_id get_device_id(int device);

    public:

    OpenCL(int device);
    ~OpenCL();

    int
    init(int width, int height);

    int
    do_frame(const uint8_t* pixels, AVFrame *encoder_frame, AVPixelFormat format, bool y_invert);
};
