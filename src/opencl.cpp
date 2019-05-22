#include <iostream>

#include "opencl.hpp"

static char const *cl_source_str = "									\n\
__kernel void rgbx_2_yuv (__global  unsigned int * sourceImage,						\n\
                          __global unsigned int * destImage,						\n\
                          unsigned int srcWidth,							\n\
                          unsigned int srcHeight,							\n\
                          short rgb0)									\n\
{													\n\
    int i,j;												\n\
    unsigned int pixels[4];										\n\
    unsigned int posSrc, RGB, Y = 0, UV = 0, ValueY, ValueU, ValueV;					\n\
    unsigned char r, g, b;										\n\
													\n\
    unsigned int posX = get_global_id(0);								\n\
    unsigned int posY = get_global_id(1);								\n\
    unsigned int yuvStride = srcWidth >> 2;								\n\
													\n\
    if (posX >= yuvStride || posY >= srcHeight)								\n\
        return;												\n\
													\n\
    posSrc = (posY * srcWidth) + (posX * 4);								\n\
													\n\
    pixels[0] = sourceImage [posSrc + 0];								\n\
    pixels[1] = sourceImage [posSrc + 1];								\n\
    pixels[2] = sourceImage [posSrc + 2];								\n\
    pixels[3] = sourceImage [posSrc + 3];								\n\
													\n\
    for (i = 0; i < 4; i++)										\n\
    {													\n\
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
        // Y plane - pack 4 * 8-bit Y's within each 32-bit unit.					\n\
        // Each 24-bit RGB value is sampled and converted into an					\n\
        // 8-bit Y value which represent the pixels as grayscale.					\n\
        ValueY = ((66 * r + 129 * g + 25 * b) >> 8) + 16;						\n\
        Y |= (ValueY << (i * 8));									\n\
													\n\
        // UV plane - pack 1 * 8-bit U and 1 * 8-bit V for each sampled pixel				\n\
        // In this case the target is nv12/yuv420 which means we only sample				\n\
        // every other row and every other pixel in each row						\n\
        if (!(posY % 2) && !(i % 2))									\n\
        {												\n\
            ValueU = ((-38 * r + -74 * g + 112 * b) >> 8) + 128;					\n\
            ValueV = ((112 * r - 94 * g - 18 * b) >> 8) + 128;						\n\
            UV |= (ValueU << (i * 8));									\n\
            UV |= (ValueV << ((i + 1) * 8));								\n\
        }												\n\
    }													\n\
													\n\
    destImage [(posY * yuvStride) + posX] = Y;								\n\
    if (!(posY % 2))											\n\
        destImage [(yuvStride * srcHeight) + ((posY >> 1) * yuvStride) + posX] = UV;			\n\
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
    cl_int ret = clGetPlatformIDs(1, &platform_id, &ret_num_platforms);
    ret = clGetDeviceIDs( platform_id, CL_DEVICE_TYPE_DEFAULT, 1,
        &device_id, &ret_num_devices);
    if (ret)
    {
        std::cerr << "clGetDeviceIDs failed!" << std::endl;
        return;
    }

    // Create an OpenCL context
    context = clCreateContext( NULL, 1, &device_id, NULL, NULL, &ret);
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
    kernel = clCreateKernel(program, "rgbx_2_yuv", &ret);
    if (ret)
    {
        std::cerr << "clCreateKernel failed!" << std::endl;
        return;
    }

    unsigned int frameSize = width * height;
    argbSize = frameSize * 4; // ARGB pixels

    yuvSize = frameSize + (frameSize >> 1); // Y+UV planes

    yuvStride = width >> 2; // since we pack 4 RGBs into "one" YYYY

    yuv_buffer = clCreateBuffer ( context, CL_MEM_WRITE_ONLY, yuvSize * sizeof(uint32_t), 0, &ret );
    if (ret)
    {
        std::cerr << "clCreateBuffer (yuv) failure!" << std::endl;
        return;
    }

    local_yuv_buffer = (uint32_t *) malloc(yuvSize * sizeof(uint32_t));

    if (!local_yuv_buffer)
        std::cerr << "malloc failure!" << std::endl;

    std::cout << "Using OpenCL for accelerated RGB to YUV conversion" << std::endl;
}

int
OpenCL::do_frame(const uint8_t* pixels, AVFrame *encoder_frame, AVPixelFormat format, bool y_invert)
{
    const uint8_t *formatted_pixels = pixels;
    short rgb0 = format == AV_PIX_FMT_RGB0 ? 1 : 0;

    if (ret)
        return ret;

    rgb_buffer = clCreateBuffer ( context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, argbSize, (void *) pixels, &ret );
    if (ret)
    {
        std::cerr << "clCreateBuffer (rgb) failed!" << std::endl;
        return -1;
    }

    ret |= clSetKernelArg ( kernel, 0, sizeof(cl_mem), &rgb_buffer );
    ret |= clSetKernelArg ( kernel, 1, sizeof(cl_mem), &yuv_buffer );
    ret |= clSetKernelArg ( kernel, 2, sizeof(unsigned int), &width);
    ret |= clSetKernelArg ( kernel, 3, sizeof(unsigned int), &height);
    ret |= clSetKernelArg ( kernel, 4, sizeof(short), &rgb0);
    if (ret)
    {
        std::cerr << "clSetKernelArg failed!" << std::endl;
        return -1;
    }

    const size_t global_ws[] = { yuvStride + (yuvStride >> 1), size_t(height) };
    ret |= clEnqueueNDRangeKernel ( command_queue, kernel, 2, NULL, global_ws, NULL, 0, NULL, NULL );
    if (ret)
    {
        std::cerr << "clEnqueueNDRangeKernel failed!" << std::endl;
        return -1;
    }

    // Read yuv buffer from gpu
    ret |= clEnqueueReadBuffer(command_queue, yuv_buffer, CL_TRUE, 0,
        yuvSize * sizeof(uint32_t), local_yuv_buffer, 0, NULL, NULL);
    if (ret)
    {
        std::cerr << "clEnqueueReadBuffer failed!" << std::endl;
        return -1;
    }

    formatted_pixels = (uint8_t *) local_yuv_buffer;
    if (y_invert)
        formatted_pixels += width * (height - 1);

    encoder_frame->data[0] = (uint8_t *) formatted_pixels;

    if (y_invert)
        formatted_pixels += width * height >> 1;
    else
        formatted_pixels += width * height;

    encoder_frame->data[1] = (uint8_t *) formatted_pixels;

    encoder_frame->linesize[0] = -width;
    encoder_frame->linesize[1] = -width;

    return ret;
}

OpenCL::~OpenCL()
{
    clFlush(command_queue);
    clFinish(command_queue);
    clReleaseKernel(kernel);
    clReleaseProgram(program);
    clReleaseMemObject(yuv_buffer);
    clReleaseCommandQueue(command_queue);
    clReleaseContext(context);
    free(local_yuv_buffer);
}