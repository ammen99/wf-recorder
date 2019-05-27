/*
 * Adapted from an example found here https://stackoverflow.com/questions/4979504/fast-rgb-yuv-conversion-in-opencl
 * Copyright 2019 Scott Moreau
 *
 */

#include <iostream>

#include "opencl.hpp"


static char const *cl_source_str = "									\n\
__kernel void rgbx_2_yuv420 (__global unsigned int  *sourceImage,					\n\
                             __global unsigned char *destImage,						\n\
                             unsigned int srcWidth,							\n\
                             unsigned int srcHeight,							\n\
                             short rgb0)								\n\
{													\n\
    int i, d;												\n\
    unsigned int pixels[4], posSrc[2];									\n\
    unsigned int RGB, ValueY, ValueU, ValueV, c1, c2, c3;						\n\
    unsigned char r, g, b;										\n\
													\n\
    unsigned int posX = get_global_id(0);								\n\
    unsigned int posY = get_global_id(1);								\n\
													\n\
    unsigned int halfWidth = ((srcWidth + 1) >> 1);							\n\
    unsigned int halfHeight = ((srcHeight + 1) >> 1);							\n\
													\n\
    if (posX >= halfWidth || posY >= halfHeight)							\n\
        return;												\n\
													\n\
    posSrc[0] = ((posY * 2) * srcWidth) + (posX * 2);							\n\
    posSrc[1] = (((posY * 2) + 1) * srcWidth) + (posX * 2);						\n\
													\n\
    pixels[0] = sourceImage[posSrc[0] + 0];								\n\
    pixels[1] = sourceImage[posSrc[0] + 1];								\n\
    pixels[2] = sourceImage[posSrc[1] + 0];								\n\
    pixels[3] = sourceImage[posSrc[1] + 1];								\n\
													\n\
    for (i = 0; i < 4; i++)										\n\
    {													\n\
        if (i == 1 && (posX + 1) > halfWidth)								\n\
            continue;											\n\
        if (i > 1 && ((posSrc[1] + ((i - 1) >> 1)) >= (srcWidth * srcHeight)))				\n\
            break;											\n\
													\n\
        RGB = pixels[i];										\n\
        if (rgb0)											\n\
        {												\n\
            r = (RGB) & 0xff; g = (RGB >> 8) & 0xff; b = (RGB >> 16) & 0xff;				\n\
        }												\n\
        else //bgr0											\n\
        {												\n\
            b = (RGB) & 0xff; g = (RGB >> 8) & 0xff; r = (RGB >> 16) & 0xff;				\n\
        }												\n\
													\n\
        // Y plane - pack 1 * 8-bit Y's within each 8-bit unit.						\n\
        ValueY = ((66 * r + 129 * g + 25 * b) >> 8) + 16;						\n\
        if (i < 2)											\n\
        {												\n\
            destImage[((posY * 2) * srcWidth) + (posX * 2) + i] = ValueY;				\n\
        }												\n\
        else												\n\
        {												\n\
            destImage[(((posY * 2) + 1) * srcWidth) + (posX * 2) + (i - 2)] = ValueY;			\n\
        }												\n\
    }													\n\
													\n\
    c1 = (pixels[0] & 0xff);										\n\
    c2 = ((pixels[0] >> 8) & 0xff);									\n\
    c3 = ((pixels[0] >> 16) & 0xff);									\n\
    d = 0;												\n\
    if (posX + 1 == halfWidth)										\n\
    {													\n\
        c1 += (pixels[1] & 0xff);									\n\
        c2 += ((pixels[1] >> 8) & 0xff);								\n\
        c3 += ((pixels[1] >> 16) & 0xff);								\n\
        d++;												\n\
    }													\n\
    if (posY + 1 == halfHeight)										\n\
    {													\n\
        c1 += (pixels[2] & 0xff);									\n\
        c2 += ((pixels[2] >> 8) & 0xff);								\n\
        c3 += ((pixels[2] >> 16) & 0xff);								\n\
        d++;												\n\
    }													\n\
    if (posX + 1 == halfWidth && posY + 1 == halfHeight)						\n\
    {													\n\
        c1 += (pixels[3] & 0xff);									\n\
        c2 += ((pixels[3] >> 8) & 0xff);								\n\
        c3 += ((pixels[3] >> 16) & 0xff);								\n\
    }													\n\
    if (rgb0)												\n\
    {													\n\
        r = c1 >> d; g = c2 >> d; b = c3 >> d;								\n\
    }													\n\
    else //bgr0												\n\
    {													\n\
        b = c1 >> d; g = c2 >> d; r = c3 >> d;								\n\
    }													\n\
													\n\
    // UV plane - pack 1 * 8-bit U and 1 * 8-bit V for each 4-pixel average				\n\
    ValueU = ((-38 * r + -74 * g + 112 * b) >> 8) + 128;						\n\
    ValueV = ((112 * r - 94 * g - 18 * b) >> 8) + 128;							\n\
    unsigned int u_offset = (srcWidth * srcHeight) + (posY * halfWidth);				\n\
    unsigned int v_offset = u_offset + (halfWidth * halfHeight);					\n\
    destImage[u_offset + posX] = ValueU;								\n\
    destImage[v_offset + posX] = ValueV;								\n\
    return;												\n\
}													\n\
";

OpenCL::OpenCL(int _width, int _height)
{
    width = _width;
    height = _height;

    // Get platform and device information
    cl_platform_id platform_id = NULL;
    cl_device_id device_id = NULL;
    cl_uint ret_num_devices;
    cl_uint ret_num_platforms;
    ret = clGetPlatformIDs(1, &platform_id, &ret_num_platforms);
    if (ret)
    {
        std::cerr << "clGetPlatformIDs failed!" << std::endl;
        return;
    }
    ret = clGetDeviceIDs(platform_id, CL_DEVICE_TYPE_DEFAULT, 1,
        &device_id, &ret_num_devices);
    if (ret)
    {
        std::cerr << "clGetDeviceIDs failed!" << std::endl;
        return;
    }

    // Create an OpenCL context
    context = clCreateContext(NULL, 1, &device_id, NULL, NULL, &ret);
    if (ret)
    {
        std::cerr << "clCreateContext failed!" << std::endl;
        return;
    }

    // Create a command queue
    command_queue = clCreateCommandQueue(context, device_id, 0, &ret);
    if (ret)
    {
        std::cerr << "clCreateCommandQueue failed!" << std::endl;
        return;
    }
    // Create a program from the kernel source
    program = clCreateProgramWithSource(context, 1,
        (const char **)&cl_source_str, NULL, &ret);
    if (ret)
    {
        std::cerr << "clCreateProgramWithSource failed!" << std::endl;
        return;
    }

    // Build the program
    ret |= clBuildProgram(program, 1, &device_id, NULL, NULL, NULL);
    if (ret)
    {
        std::cerr << "clBuildProgram failed!" << std::endl;

        char *build_log;
        size_t ret_val_size;
        clGetProgramBuildInfo(program, device_id, CL_PROGRAM_BUILD_LOG, 0, NULL, &ret_val_size);
        build_log = new char[ret_val_size+1];
        clGetProgramBuildInfo(program, device_id, CL_PROGRAM_BUILD_LOG, ret_val_size, build_log, NULL);
        std::cout << build_log << std::endl;
        delete build_log;
    }

    // Create the OpenCL kernel
    kernel = clCreateKernel(program, "rgbx_2_yuv420", &ret);
    if (ret)
    {
        std::cerr << "clCreateKernel failed!" << std::endl;
        return;
    }

    halfWidth = ((width + 1) >> 1);
    halfHeight = ((height + 1) >> 1);
    unsigned int frameSize = width * height;
    unsigned int frameSizeUV = halfWidth * halfHeight;

    argbSize = frameSize * 4; // ARGB pixels

    yuv420Size = frameSize + frameSizeUV * 2; // Y+UV planes

    yuv420_buffer = clCreateBuffer(context, CL_MEM_WRITE_ONLY, yuv420Size * sizeof(char) * 4, 0, &ret);
    if (ret)
    {
        std::cerr << "clCreateBuffer (yuv420) failure!" << std::endl;
        return;
    }

    local_yuv420_buffer = (uint8_t *) malloc(yuv420Size * sizeof(uint8_t) * 4);

    if (!local_yuv420_buffer)
        std::cerr << "malloc failure!" << std::endl;

    std::cout << "Using OpenCL for accelerated RGB to YUV420 conversion" << std::endl;
}

int
OpenCL::do_frame(const uint8_t* pixels, AVFrame *encoder_frame, AVPixelFormat format, bool y_invert)
{
    const uint8_t *formatted_pixels = pixels;
    short rgb0 = format == AV_PIX_FMT_RGB0 ? 1 : 0;

    if (ret)
        return ret;

    rgb_buffer = clCreateBuffer(context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, argbSize, (void *) pixels, &ret);
    if (ret)
    {
        std::cerr << "clCreateBuffer (rgb) failed!" << std::endl;
        return -1;
    }

    ret |= clSetKernelArg ( kernel, 0, sizeof(cl_mem), &rgb_buffer );
    ret |= clSetKernelArg ( kernel, 1, sizeof(cl_mem), &yuv420_buffer );
    ret |= clSetKernelArg ( kernel, 2, sizeof(unsigned int), &width);
    ret |= clSetKernelArg ( kernel, 3, sizeof(unsigned int), &height);
    ret |= clSetKernelArg ( kernel, 4, sizeof(short), &rgb0);
    if (ret)
    {
        std::cerr << "clSetKernelArg failed!" << std::endl;
        return -1;
    }

    const size_t global_ws[] = {halfWidth, halfHeight};
    ret |= clEnqueueNDRangeKernel(command_queue, kernel, 2, NULL, global_ws, NULL, 0, NULL, NULL);
    if (ret)
    {
        std::cerr << "clEnqueueNDRangeKernel failed!" << std::endl;
        return -1;
    }

    // Read yuv420 buffer from gpu
    ret |= clEnqueueReadBuffer(command_queue, yuv420_buffer, CL_TRUE, 0,
        yuv420Size * sizeof(uint8_t) * 4, local_yuv420_buffer, 0, NULL, NULL);
    if (ret)
    {
        std::cerr << "clEnqueueReadBuffer failed!" << std::endl;
        return -1;
    }

    ret |= clReleaseMemObject(rgb_buffer);
    if (ret)
    {
        std::cerr << "clReleaseMemObject failed!" << std::endl;
        return -1;
    }

    formatted_pixels = local_yuv420_buffer;

    if (y_invert)
        formatted_pixels += width * (height - 1);
    encoder_frame->data[0] = (uint8_t *) formatted_pixels;

    if (y_invert)
        formatted_pixels += (halfWidth) * (halfHeight - 1) + width;
    else
        formatted_pixels += width * height;
    encoder_frame->data[1] = (uint8_t *) formatted_pixels;

    formatted_pixels += halfWidth * halfHeight;
    encoder_frame->data[2] = (uint8_t *) formatted_pixels;

    short flip = y_invert ? -1 : 1;

    encoder_frame->linesize[0] = width * flip;
    encoder_frame->linesize[1] = halfWidth * flip;
    encoder_frame->linesize[2] = halfWidth * flip;

    return ret;
}

OpenCL::~OpenCL()
{
    clFlush(command_queue);
    clFinish(command_queue);
    clReleaseKernel(kernel);
    clReleaseProgram(program);
    clReleaseMemObject(yuv420_buffer);
    /* Causes crash */
    //clReleaseCommandQueue(command_queue);
    clReleaseContext(context);
    free(local_yuv420_buffer);
}