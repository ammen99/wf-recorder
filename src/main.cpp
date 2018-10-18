#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 199309L

#include <string>
#include <thread>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-client-protocol.h>

#include "ipc.hpp"
#include "wlr-screencopy-unstable-v1-client-protocol.h"

struct format {
	enum wl_shm_format wl_format;
	bool is_bgr;
};

static struct wl_shm *shm = NULL;
static struct zwlr_screencopy_manager_v1 *screencopy_manager = NULL;
static struct wl_output *output = NULL;

static struct {
	struct wl_buffer *wl_buffer;
	void *data;
	enum wl_shm_format format;
	int width, height, stride;
	bool y_invert;
    timespec presented;
} buffer;
bool buffer_copy_done = false;

static const struct format formats[] = {
	{WL_SHM_FORMAT_XRGB8888, true},
	{WL_SHM_FORMAT_ARGB8888, true},
	{WL_SHM_FORMAT_XBGR8888, false},
	{WL_SHM_FORMAT_ABGR8888, false},
};

static int backingfile(off_t size)
{
	char name[] = "/tmp/wf-recorder-shared-XXXXXX";
	int fd = mkstemp(name);
	if (fd < 0) {
		return -1;
	}

	int ret;
	while ((ret = ftruncate(fd, size)) == EINTR) {
		// No-op
	}
	if (ret < 0) {
		close(fd);
		return -1;
	}

	unlink(name);
	return fd;
}

static struct wl_buffer *create_shm_buffer(uint32_t fmt,
		int width, int height, int stride, void **data_out)
{
	int size = stride * height;

	int fd = backingfile(size);
	if (fd < 0) {
		fprintf(stderr, "creating a buffer file for %d B failed: %m\n", size);
		return NULL;
	}

	void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		fprintf(stderr, "mmap failed: %m\n");
		close(fd);
		return NULL;
	}

	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	close(fd);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, width, height,
		stride, fmt);
	wl_shm_pool_destroy(pool);

	*data_out = data;
	return buffer;
}

static void frame_handle_buffer(void *, struct zwlr_screencopy_frame_v1 *frame, uint32_t format,
		uint32_t width, uint32_t height, uint32_t stride)
{
	buffer.format = (wl_shm_format)format;
	buffer.width = width;
	buffer.height = height;
    buffer.stride = stride;
    if (!buffer.wl_buffer) {
        buffer.wl_buffer =
            create_shm_buffer(format, width, height, stride, &buffer.data);
    }
	if (buffer.wl_buffer == NULL) {
		fprintf(stderr, "failed to create buffer\n");
		exit(EXIT_FAILURE);
	}

	zwlr_screencopy_frame_v1_copy(frame, buffer.wl_buffer);
}

static void frame_handle_flags(void*, struct zwlr_screencopy_frame_v1 *, uint32_t flags) {
	buffer.y_invert = flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT;
}

static void frame_handle_ready(void *, struct zwlr_screencopy_frame_v1 *, uint32_t tv_sec_hi, uint32_t tv_sec_low, uint32_t tv_nsec) {
	buffer_copy_done = true;
    buffer.presented.tv_sec = ((1ll * tv_sec_hi) << 32ll) | tv_sec_low;
    buffer.presented.tv_nsec = tv_nsec;
}

static void frame_handle_failed(void *, struct zwlr_screencopy_frame_v1 *) {
	fprintf(stderr, "failed to copy frame\n");
	exit(EXIT_FAILURE);
}

static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
	.buffer = frame_handle_buffer,
	.flags = frame_handle_flags,
	.ready = frame_handle_ready,
	.failed = frame_handle_failed,
};

static void handle_global(void*, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t) {
	if (strcmp(interface, wl_output_interface.name) == 0 && output == NULL) {
		output = (wl_output*)wl_registry_bind(registry, name, &wl_output_interface, 1);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		shm = (wl_shm*) wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface,
			zwlr_screencopy_manager_v1_interface.name) == 0) {
		screencopy_manager = (zwlr_screencopy_manager_v1*) wl_registry_bind(registry, name,
			&zwlr_screencopy_manager_v1_interface, 1);
	}
}

static void handle_global_remove(void*, struct wl_registry *, uint32_t) {
	// Who cares?
}

static const struct wl_registry_listener registry_listener = {
	.global = handle_global,
	.global_remove = handle_global_remove,
};

static uint64_t timespec_to_msec (const timespec& ts)
{
    return ts.tv_sec * 1000ll + 1ll * ts.tv_nsec / 1000000ll;
}

static void write_buffer(uint32_t present_msec, int fd)
{
    uint8_t header[4];
    header[0] = present_msec >> 24;
    header[1] = (present_msec >> 16) & 0xff;
    header[2] = (present_msec >> 8) & 0xff;
    header[3] = (present_msec & 0xff);

    write(fd, header, 4);
    write(fd, buffer.data, buffer.width * buffer.height * 4);
}

int main()
{
	struct wl_display * display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "failed to create display: %m\n");
		return EXIT_FAILURE;
	}

	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_dispatch(display);
	wl_display_roundtrip(display);

	if (shm == NULL) {
		fprintf(stderr, "compositor is missing wl_shm\n");
		return EXIT_FAILURE;
	}
	if (screencopy_manager == NULL) {
		fprintf(stderr, "compositor doesn't support wlr-screencopy-unstable-v1\n");
		return EXIT_FAILURE;
	}
	if (output == NULL) {
		fprintf(stderr, "no output available\n");
		return EXIT_FAILURE;
	}

    int fd[2];
    pipe(fd);

    std::thread writer_thread([=] () {
        write_loop(1920, 1080, 4, fd[0]);
    });

    timespec ts;
    ts.tv_sec = -1;

    int stop = 500;
    while(true)
    {

        buffer_copy_done = false;
        struct zwlr_screencopy_frame_v1 *frame =
            zwlr_screencopy_manager_v1_capture_output(screencopy_manager, 0, output);
        zwlr_screencopy_frame_v1_add_listener(frame, &frame_listener, NULL);

        while (!buffer_copy_done && wl_display_dispatch(display) != -1) {
            // This space is intentionally left blank
        }

        if (ts.tv_sec == -1)
            ts = buffer.presented;

        uint32_t present_msec = timespec_to_msec(buffer.presented)
            - timespec_to_msec(ts);

        write_buffer(fd[1], present_msec);
        zwlr_screencopy_frame_v1_destroy(frame);

        if (!(stop--))
            break;
    }

    writer_thread.join();
	return EXIT_SUCCESS;
}
