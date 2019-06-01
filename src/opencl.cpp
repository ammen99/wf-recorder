/*
 * Adapted from an example found here https://stackoverflow.com/questions/4979504/fast-rgb-yuv-conversion-in-opencl
 * Copyright 2019 Scott Moreau
 *
 */

#include <iostream>

#include "opencl.hpp"


static char const *cl_source_str = R"(
__kernel void rgbx_2_yuv420 (__global unsigned int  *sourceImage,
                             __global unsigned char *destImage,
                             unsigned int srcWidth,
                             unsigned int srcHeight,
                             short rgb0)
{
    int i, d;
    unsigned int pixels[4], posSrc[2];
    unsigned int RGB, ValueY, ValueU, ValueV, c1, c2, c3, u_offset, v_offset;
    unsigned char r, g, b;

    unsigned int posX = get_global_id(0);
    unsigned int posY = get_global_id(1);

    unsigned int X2 = posX * 2;
    unsigned int Y2 = posY * 2;

    unsigned int size = srcWidth * srcHeight;

    unsigned int halfWidth = ((srcWidth + 1) >> 1);
    unsigned int halfHeight = ((srcHeight + 1) >> 1);

    if (posX >= halfWidth || posY >= halfHeight)
        return;

    posSrc[0] = (Y2 * srcWidth) + X2;
    posSrc[1] = ((Y2 + 1) * srcWidth) + X2;

    pixels[0] = sourceImage[posSrc[0] + 0];
    pixels[1] = sourceImage[posSrc[0] + 1];
    pixels[2] = sourceImage[posSrc[1] + 0];
    pixels[3] = sourceImage[posSrc[1] + 1];

    for (i = 0; i < 4; i++)
    {
        if (i == 1 && (X2 + 1) >= srcWidth)
            continue;
        if (i > 1 && (posSrc[1] + ((i - 1) >> 1)) >= size)
            break;

        RGB = pixels[i];
        if (rgb0)
        {
            r = (RGB) & 0xff; g = (RGB >> 8) & 0xff; b = (RGB >> 16) & 0xff;
        }
        else //bgr0
        {
            b = (RGB) & 0xff; g = (RGB >> 8) & 0xff; r = (RGB >> 16) & 0xff;
        }

        // Y plane - pack 1 * 8-bit Y within each 8-bit unit.
        ValueY = ((66 * r + 129 * g + 25 * b) >> 8) + 16;
        if (i < 2)
        {
            destImage[(Y2 * srcWidth) + X2 + i] = ValueY;
        }
        else
        {
            destImage[((Y2 + 1) * srcWidth) + X2 + (i - 2)] = ValueY;
        }
    }

    c1 = (pixels[0] & 0xff);
    c2 = ((pixels[0] >> 8) & 0xff);
    c3 = ((pixels[0] >> 16) & 0xff);
    d = 0;
    if ((X2 + 1) < srcWidth)
    {
        c1 += (pixels[1] & 0xff);
        c2 += ((pixels[1] >> 8) & 0xff);
        c3 += ((pixels[1] >> 16) & 0xff);
        d++;
    }
    if ((Y2 + 1) < srcHeight)
    {
        c1 += (pixels[2] & 0xff);
        c2 += ((pixels[2] >> 8) & 0xff);
        c3 += ((pixels[2] >> 16) & 0xff);
        d++;
    }
    if (d == 2)
    {
        c1 += (pixels[3] & 0xff);
        c2 += ((pixels[3] >> 8) & 0xff);
        c3 += ((pixels[3] >> 16) & 0xff);
    }
    if (rgb0)
    {
        r = c1 >> d; g = c2 >> d; b = c3 >> d;
    }
    else //bgr0
    {
        b = c1 >> d; g = c2 >> d; r = c3 >> d;
    }

    // UV plane - pack 1 * 8-bit U and 1 * 8-bit V for each subsample average
    ValueU = ((-38 * r - 74 * g + 112 * b) >> 8) + 128;
    ValueV = ((112 * r - 94 * g - 18  * b) >> 8) + 128;

    u_offset = size + (posY * halfWidth);
    v_offset = u_offset + (halfWidth * halfHeight);

    destImage[u_offset + posX] = ValueU;
    destImage[v_offset + posX] = ValueV;

    return;
}
)";

cl_device_id
OpenCL::get_device_id(int device)
{
    uint32_t i, j;
    char* value;
    size_t valueSize;
    cl_uint platformCount;
    cl_platform_id* platforms;
    cl_uint deviceCount;
    cl_device_id* devices;
    cl_device_id device_id;
    std::vector<cl_device_id> all_devices;

    ret = clGetPlatformIDs(0, NULL, &platformCount);
    if (ret)
    {
        std::cerr << "clGetPlatformIDs failed!" << std::endl;
        return NULL;
    }
    if (!platformCount)
    {
        std::cerr << "No OpenCL platforms detected." << std::endl;
        return NULL;
    }
    platforms = (cl_platform_id*) malloc(sizeof(cl_platform_id) * platformCount);
    ret = clGetPlatformIDs(platformCount, platforms, NULL);
    if (ret)
    {
        std::cerr << "clGetPlatformIDs failed!" << std::endl;
        return NULL;
    }

    if (platformCount == 1 && device <= 0)
    {
        ret = clGetDeviceIDs(platforms[0], CL_DEVICE_TYPE_ALL, 0, NULL, &deviceCount);
        if (ret)
        {
            std::cerr << "clGetDeviceIDs failed!" << std::endl;
            return NULL;
        }
        if (!deviceCount)
        {
            std::cerr << "No OpenCL devices detected." << std::endl;
            return NULL;
        }
        if (deviceCount == 1)
        {
            ret = clGetDeviceIDs(platforms[0], CL_DEVICE_TYPE_DEFAULT, 1,
                &device_id, &deviceCount);
            if (ret)
            {
                std::cerr << "clGetDeviceIDs failed!" << std::endl;
                return NULL;
            }
            return device_id;
	}
    }

    if (device < 0)
    {
        std::cout << std::endl;
        std::cout << "Please choose an OpenCL device:" << std::endl;
    }

    for (i = 0; i < platformCount; i++) {
        ret = clGetDeviceIDs(platforms[i], CL_DEVICE_TYPE_ALL, 0, NULL, &deviceCount);
        if (ret)
        {
            std::cerr << "clGetDeviceIDs failed!" << std::endl;
            return NULL;
        }
        devices = (cl_device_id*) malloc(sizeof(cl_device_id) * deviceCount);
        ret = clGetDeviceIDs(platforms[i], CL_DEVICE_TYPE_ALL, deviceCount, devices, NULL);
        if (ret)
        {
            std::cerr << "clGetDeviceIDs failed!" << std::endl;
            return NULL;
        }

        for (j = 0; j < deviceCount; j++) {
            ret = clGetDeviceInfo(devices[j], CL_DEVICE_NAME, 0, NULL, &valueSize);
            if (ret)
            {
                std::cerr << "clGetDeviceInfo failed!" << std::endl;
                return NULL;
            }
            value = (char*) malloc(valueSize);
            ret = clGetDeviceInfo(devices[j], CL_DEVICE_NAME, valueSize, value, NULL);
            if (ret)
            {
                std::cerr << "clGetDeviceInfo failed!" << std::endl;
                return NULL;
            }
            all_devices.push_back(devices[j]);
            if (device < 0)
                std::cout << all_devices.size() << ": " << value << std::endl;
            free(value);
            if (device == (int) all_devices.size())
                break;
        }

        free(devices);
        if (device == (int) all_devices.size())
            break;
    }

    free(platforms);

    if (device > (int) all_devices.size())
    {
        std::cerr << "Max OpenCL device number is " << all_devices.size() << std::endl;
        return NULL;
    }

    if (!device)
        return all_devices[device];

    if (device > 0)
        return all_devices[device - 1];

    std::cout << "Enter device no.:";
    fflush(stdout);

    int choice;
    if (scanf("%d", &choice) != 1 || choice > (int) all_devices.size() || choice <= 0)
    {
        std::cerr << "Bad choice." << std::endl;
        return NULL;
    }

    return all_devices[choice - 1];
}

int
OpenCL::init(int _width, int _height)
{
    if (ret)
        return ret;

    width = _width;
    height = _height;

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
        return ret;
    }

    local_yuv420_buffer = (uint8_t *) malloc(yuv420Size * sizeof(uint8_t) * 4);

    if (!local_yuv420_buffer)
    {
        std::cerr << "malloc failure!" << std::endl;
        ret = -1;
    }

    std::cout << "Using OpenCL for accelerated RGB to YUV420 conversion" << std::endl;

    return ret;
}

OpenCL::OpenCL(int device)
{
    device_id = get_device_id(device);
    if (!device_id)
    {
        ret = -1;
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
        return ret;
    }

    ret |= clSetKernelArg ( kernel, 0, sizeof(cl_mem), &rgb_buffer );
    ret |= clSetKernelArg ( kernel, 1, sizeof(cl_mem), &yuv420_buffer );
    ret |= clSetKernelArg ( kernel, 2, sizeof(unsigned int), &width);
    ret |= clSetKernelArg ( kernel, 3, sizeof(unsigned int), &height);
    ret |= clSetKernelArg ( kernel, 4, sizeof(short), &rgb0);
    if (ret)
    {
        std::cerr << "clSetKernelArg failed!" << std::endl;
        return ret;
    }

    const size_t global_ws[] = {halfWidth, halfHeight};
    ret |= clEnqueueNDRangeKernel(command_queue, kernel, 2, NULL, global_ws, NULL, 0, NULL, NULL);
    if (ret)
    {
        std::cerr << "clEnqueueNDRangeKernel failed!" << std::endl;
        return ret;
    }

    // Read yuv420 buffer from gpu
    ret |= clEnqueueReadBuffer(command_queue, yuv420_buffer, CL_TRUE, 0,
        yuv420Size * sizeof(uint8_t) * 4, local_yuv420_buffer, 0, NULL, NULL);
    if (ret)
    {
        std::cerr << "clEnqueueReadBuffer failed!" << std::endl;
        return ret;
    }

    ret |= clReleaseMemObject(rgb_buffer);
    if (ret)
    {
        std::cerr << "clReleaseMemObject failed!" << std::endl;
        return ret;
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
    if (ret)
        return;

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