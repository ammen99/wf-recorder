#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 199309L

#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <getopt.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <signal.h>
#include <unistd.h>
#include <wayland-client-protocol.h>

#include "frame-writer.hpp"
#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"

static struct wl_shm *shm = NULL;
static struct zxdg_output_manager_v1 *xdg_output_manager = NULL;
static struct zwlr_screencopy_manager_v1 *screencopy_manager = NULL;

struct wf_recorder_output
{
    wl_output *output;
    zxdg_output_v1 *zxdg_output;
    std::string name, description;
};

std::vector<wf_recorder_output> available_outputs;

static void handle_xdg_output_logical_position(void*, zxdg_output_v1*, int32_t, int32_t) { }
static void handle_xdg_output_logical_size(void*, zxdg_output_v1*, int32_t, int32_t) { }
static void handle_xdg_output_done(void*, zxdg_output_v1*) { }

static void handle_xdg_output_name(void*, zxdg_output_v1 *zxdg_output_v1,
    const char *name)
{
    for (auto& wo : available_outputs)
    {
        if (wo.zxdg_output == zxdg_output_v1)
            wo.name = name;
    }
}

static void handle_xdg_output_description(void*, zxdg_output_v1 *zxdg_output_v1,
    const char *description)
{
    for (auto& wo : available_outputs)
    {
        if (wo.zxdg_output == zxdg_output_v1)
            wo.description = description;
    }
}


const zxdg_output_v1_listener xdg_output_implementation = {
    .logical_position = handle_xdg_output_logical_position,
    .logical_size = handle_xdg_output_logical_size,
    .done = handle_xdg_output_done,
    .name = handle_xdg_output_name,
    .description = handle_xdg_output_description
};

struct wf_buffer
{
	struct wl_buffer *wl_buffer;
	void *data;
	enum wl_shm_format format;
	int width, height, stride;
	bool y_invert;

    timespec presented;
    uint32_t base_msec;

    std::atomic<bool> released{true}; // if the buffer can be used to store new pending frames
    std::atomic<bool> available{false}; // if the buffer can be used to feed the encoder
};

std::atomic<bool> exit_main_loop{false};

#define MAX_BUFFERS 16
wf_buffer buffers[MAX_BUFFERS];
size_t active_buffer = 0;

bool buffer_copy_done = false;

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
    auto& buffer = buffers[active_buffer];

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
	buffers[active_buffer].y_invert = flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT;
}

static void frame_handle_ready(void *, struct zwlr_screencopy_frame_v1 *,
    uint32_t tv_sec_hi, uint32_t tv_sec_low, uint32_t tv_nsec) {

    auto& buffer = buffers[active_buffer];
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
	if (strcmp(interface, wl_output_interface.name) == 0)
    {
		auto output = (wl_output*)wl_registry_bind(registry, name, &wl_output_interface, 1);
        wf_recorder_output wro;
        wro.output = output;
        available_outputs.push_back(wro);
	}
    else if (strcmp(interface, wl_shm_interface.name) == 0)
    {
		shm = (wl_shm*) wl_registry_bind(registry, name, &wl_shm_interface, 1);
	}
    else if (strcmp(interface, zwlr_screencopy_manager_v1_interface.name) == 0)
    {
		screencopy_manager = (zwlr_screencopy_manager_v1*) wl_registry_bind(registry, name,
			&zwlr_screencopy_manager_v1_interface, 1);
	}
    else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0)
    {
        xdg_output_manager = (zxdg_output_manager_v1*) wl_registry_bind(registry, name,
            &zxdg_output_manager_v1_interface, 2); // version 2 for name & description, if available
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

static int next_frame(int frame)
{
    return (frame + 1) % MAX_BUFFERS;
}

static void write_loop(std::string name, uint32_t width, uint32_t height)
{
    /* Ignore SIGINT, main loop is responsible for the exit_main_loop signal */
    sigset_t sigset;
    sigisemptyset(&sigset);
    sigaddset(&sigset, SIGINT);
    pthread_sigmask(SIG_BLOCK, &sigset, NULL);

    FrameWriter writer(name, width, height);

    int last_encoded_frame = 0;

    while(!exit_main_loop)
    {
        // wait for frame to become available
        while(buffers[last_encoded_frame].available != true) {
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }

        auto& buffer = buffers[last_encoded_frame];
        writer.add_frame((unsigned char*)buffer.data, buffer.base_msec,
            buffer.y_invert);

        buffer.available = false;
        buffer.released = true;

        last_encoded_frame = next_frame(last_encoded_frame);
    }
}

void handle_sigint(int)
{
    exit_main_loop = true;
}

static void check_has_protos()
{
	if (shm == NULL) {
		fprintf(stderr, "compositor is missing wl_shm\n");
        exit(EXIT_FAILURE);
	}
	if (screencopy_manager == NULL) {
		fprintf(stderr, "compositor doesn't support wlr-screencopy-unstable-v1\n");
		exit(EXIT_FAILURE);
	}

    if (xdg_output_manager == NULL)
    {
        fprintf(stderr, "compositor doesn't support xdg-output-unstable-v1\n");
        exit(EXIT_FAILURE);
    }

	if (available_outputs.empty())
    {
		fprintf(stderr, "no outputs available\n");
		exit(EXIT_FAILURE);
	}
}

wl_display *display = NULL;
static void sync_wayland()
{
	wl_display_dispatch(display);
	wl_display_roundtrip(display);
}


static void load_output_info()
{
    for (auto& wo : available_outputs)
    {
        wo.zxdg_output = zxdg_output_manager_v1_get_xdg_output(
            xdg_output_manager, wo.output);
        zxdg_output_v1_add_listener(wo.zxdg_output,
            &xdg_output_implementation, NULL);
    }

    sync_wayland();
}

static wl_output *choose_interactive()
{
    fprintf(stdout, "Please select an output from the list to capture (enter output no.):\n");

    int i = 1;
    for (auto& wo : available_outputs)
    {
        printf("%d. Name: %s Description: %s\n", i++, wo.name.c_str(),
            wo.description.c_str());
    }

    printf("Enter output no.:");
    fflush(stdout);

    int choice;
    if (scanf("%d", &choice) != 1 || choice > (int)available_outputs.size() || choice <= 0)
        return NULL;

    return available_outputs[choice - 1].output;
}


int main(int argc, char *argv[])
{
    std::string file = "recording.mp4";
    std::string cmdline_output = "invalid";

    struct option opts[] = {
        { "output",          required_argument, NULL, 'o' },
        { "file",            required_argument, NULL, 'f' },
        { 0,                 0,                 NULL,  0  }
    };

    int c, i;
    while((c = getopt_long(argc, argv, "o:f:", opts, &i)) != -1)
    {
        switch(c)
        {
            case 'o':
                file = optarg;
                break;

            case 'f':
                cmdline_output = optarg;
                break;

            default:
                printf("unsupported command line argument %s\n", optarg);
        }
    }

	display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "failed to create display: %m\n");
		return EXIT_FAILURE;
	}

	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
    sync_wayland();

    check_has_protos();
    load_output_info();

    wl_output *chosen_output = NULL;
    if (available_outputs.size() == 1)
    {
        chosen_output = available_outputs[0].output;
    } else
    {
        for (auto& wo : available_outputs)
        {
            if (wo.name == cmdline_output)
                chosen_output = wo.output;
        }

        if (chosen_output == NULL)
        {
            if (cmdline_output != "invalid")
            {
                fprintf(stderr, "Couldn't find requested output %s\n",
                    cmdline_output.c_str());
            }

            chosen_output = choose_interactive();
        }
    }

    if (chosen_output == NULL)
    {
        fprintf(stderr, "Failed to select output, exiting\n");
        return EXIT_FAILURE;
    }


    timespec first_frame;
    first_frame.tv_sec = -1;

    active_buffer = 0;
    for (auto& buffer : buffers)
    {
        buffer.wl_buffer = NULL;
        buffer.available = false;
        buffer.released = true;
    }

    bool spawned_thread = false;
    std::thread writer_thread;

    signal(SIGINT, handle_sigint);

    while(!exit_main_loop)
    {
        // wait for a free buffer
        while(buffers[active_buffer].released != true) {
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }

        buffer_copy_done = false;
        struct zwlr_screencopy_frame_v1 *frame =
            zwlr_screencopy_manager_v1_capture_output(screencopy_manager, 0, chosen_output);
        zwlr_screencopy_frame_v1_add_listener(frame, &frame_listener, NULL);

        while (!buffer_copy_done && wl_display_dispatch(display) != -1) {
            // This space is intentionally left blank
        }

        auto& buffer = buffers[active_buffer];
        if (!spawned_thread)
        {
            int width = buffer.width, height = buffer.height;
            writer_thread = std::thread([=] () {
                write_loop(file, width, height);
            });

            spawned_thread = true;
        }

        if (first_frame.tv_sec == -1)
            first_frame = buffer.presented;

        buffer.base_msec = timespec_to_msec(buffer.presented)
            - timespec_to_msec(first_frame);

        buffer.released = false;
        buffer.available = true;

        active_buffer = next_frame(active_buffer);
        zwlr_screencopy_frame_v1_destroy(frame);
    }

    writer_thread.join();

    for (auto& buffer : buffers)
        wl_buffer_destroy(buffer.wl_buffer);

	return EXIT_SUCCESS;
}
