#include <unistd.h>

#include "frame-writer.hpp"
#include "ipc.hpp"

void write_loop(uint32_t width, uint32_t height, int fmt, int fd)
{
    const int data_size = width * height * fmt;
    const int presentation_size = 4;
    const int buffer_size = presentation_size + data_size;

    uint8_t buffer[buffer_size];

    uint8_t *ptr = buffer;
    uint8_t *end = buffer + buffer_size;

    FrameWriter writer("test", width, height);

    int count = 0;
    while ((count = read(fd, ptr, end - ptr)) > 0)
    {
        ptr += count;

        if (ptr != end)
            continue;

        // read a full frame
        if (ptr == end)
        {
            int present_msec = (buffer[0] << 24) | (buffer[1] << 16) |
                (buffer[2] << 8) | buffer[3];

            writer.add_frame(buffer + presentation_size, present_msec);
            ptr = buffer;
        }
    }
}
