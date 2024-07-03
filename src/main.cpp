#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 199309L
#include <iostream>
#include <optional>

#include <list>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <getopt.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <signal.h>
#include <unistd.h>
#include <wayland-client-protocol.h>
#include <gbm.h>
#include <fcntl.h>
#include <xf86drm.h>

#include "frame-writer.hpp"
#include "buffer-pool.hpp"
#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#include "linux-dmabuf-unstable-v1-client-protocol.h"

#include "config.h"

#ifdef HAVE_AUDIO
#include "audio.hpp"
AudioReaderParams audioParams;
#endif

#define MAX_FRAME_FAILURES 16

static const int GRACEFUL_TERMINATION_SIGNALS[] = { SIGTERM, SIGINT, SIGHUP };

std::mutex frame_writer_mutex, frame_writer_pending_mutex;
std::unique_ptr<FrameWriter> frame_writer;

static int drm_fd = -1;
static struct gbm_device *gbm_device = NULL;
static std::string drm_device_name;

static struct wl_shm *shm = NULL;
static struct zxdg_output_manager_v1 *xdg_output_manager = NULL;
static struct zwlr_screencopy_manager_v1 *screencopy_manager = NULL;
static struct zwp_linux_dmabuf_v1 *dmabuf = NULL;
void request_next_frame();

struct wf_recorder_output
{
    wl_output *output;
    zxdg_output_v1 *zxdg_output;
    std::string name, description;
    int32_t x, y, width, height;
};

std::list<wf_recorder_output> available_outputs;

static void handle_xdg_output_logical_position(void*,
    zxdg_output_v1* zxdg_output, int32_t x, int32_t y)
{
    for (auto& wo : available_outputs)
    {
        if (wo.zxdg_output == zxdg_output)
        {
            wo.x = x;
            wo.y = y;
        }
    }
}

static void handle_xdg_output_logical_size(void*,
    zxdg_output_v1* zxdg_output, int32_t w, int32_t h)
{
    for (auto& wo : available_outputs)
    {
        if (wo.zxdg_output == zxdg_output)
        {
            wo.width = w;
            wo.height = h;
        }
    }
}

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

struct wf_buffer : public buffer_pool_buf
{
    struct gbm_bo *bo = nullptr;
    zwp_linux_buffer_params_v1 *params = nullptr;
    struct wl_buffer *wl_buffer = nullptr;
    void *data = nullptr;
    size_t size = 0;
    enum wl_shm_format format;
    int drm_format;
    int width, height, stride;
    bool y_invert;

    timespec presented;
    uint64_t base_usec;
};

std::atomic<bool> exit_main_loop{false};

buffer_pool<wf_buffer, 16> buffers;

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

void free_shm_buffer(wf_buffer& buffer)
{
    if (buffer.wl_buffer == NULL)
    {
        return;
    }

    munmap(buffer.data, buffer.size);
    wl_buffer_destroy(buffer.wl_buffer);
    buffer.wl_buffer = NULL;
}

static bool use_damage = true;
static bool use_dmabuf = false;
static bool use_hwupload = false;

static uint32_t wl_shm_to_drm_format(uint32_t format)
{
    if (format == WL_SHM_FORMAT_ARGB8888) {
        return GBM_FORMAT_ARGB8888;
    } else if (format == WL_SHM_FORMAT_XRGB8888) {
        return GBM_FORMAT_XRGB8888;
    } else {
        return format;
    }
}

static void frame_handle_buffer(void *, struct zwlr_screencopy_frame_v1 *frame, uint32_t format,
    uint32_t width, uint32_t height, uint32_t stride)
{
    if (use_dmabuf) {
        return;
    }

    auto& buffer = buffers.capture();
    auto old_format = buffer.format;
    buffer.format = (wl_shm_format)format;
    buffer.drm_format = wl_shm_to_drm_format(format);
    buffer.width = width;
    buffer.height = height;
    buffer.stride = stride;

    /* ffmpeg requires even width and height */
    if (buffer.width % 2)
        buffer.width -= 1;
    if (buffer.height % 2)
        buffer.height -= 1;

    if (!buffer.wl_buffer || old_format != format) {
        free_shm_buffer(buffer);
        buffer.wl_buffer =
            create_shm_buffer(format, width, height, stride, &buffer.data);
    }

    if (buffer.wl_buffer == NULL) {
        fprintf(stderr, "failed to create buffer\n");
        exit(EXIT_FAILURE);
    }

    if (use_damage) {
        zwlr_screencopy_frame_v1_copy_with_damage(frame, buffer.wl_buffer);
    } else {
        zwlr_screencopy_frame_v1_copy(frame, buffer.wl_buffer);
    }
}

static void frame_handle_flags(void*, struct zwlr_screencopy_frame_v1 *, uint32_t flags) {
    buffers.capture().y_invert = flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT;
}

int32_t frame_failed_cnt = 0;

static void frame_handle_ready(void *, struct zwlr_screencopy_frame_v1 *,
    uint32_t tv_sec_hi, uint32_t tv_sec_low, uint32_t tv_nsec) {

    auto& buffer = buffers.capture();
    buffer_copy_done = true;
    buffer.presented.tv_sec = ((1ll * tv_sec_hi) << 32ll) | tv_sec_low;
    buffer.presented.tv_nsec = tv_nsec;
    frame_failed_cnt = 0;
}

static void frame_handle_failed(void *, struct zwlr_screencopy_frame_v1 *) {
    std::cerr << "Failed to copy frame, retrying..." << std::endl;
    ++frame_failed_cnt;
    request_next_frame();
    if (frame_failed_cnt > MAX_FRAME_FAILURES)
    {
        std::cerr << "Failed to copy frame too many times, exiting!" << std::endl;
        exit_main_loop = true;
    }
}

static void frame_handle_damage(void *, struct zwlr_screencopy_frame_v1 *,
    uint32_t, uint32_t, uint32_t, uint32_t)
{
}

static void dmabuf_created(void *data, struct zwp_linux_buffer_params_v1 *,
    struct wl_buffer *wl_buffer) {

    auto& buffer = buffers.capture();
    buffer.wl_buffer = wl_buffer;

    zwlr_screencopy_frame_v1 *frame = (zwlr_screencopy_frame_v1*) data;

    if (use_damage) {
        zwlr_screencopy_frame_v1_copy_with_damage(frame, buffer.wl_buffer);
    } else {
        zwlr_screencopy_frame_v1_copy(frame, buffer.wl_buffer);
    }
}

static void dmabuf_failed(void *, struct zwp_linux_buffer_params_v1 *) {
    std::cerr << "Failed to create dmabuf" << std::endl;
    exit_main_loop = true;
}

static const struct zwp_linux_buffer_params_v1_listener params_listener = {
    .created = dmabuf_created,
    .failed = dmabuf_failed,
};

static wl_shm_format drm_to_wl_shm_format(uint32_t format)
{
    if (format == GBM_FORMAT_ARGB8888) {
        return WL_SHM_FORMAT_ARGB8888;
    } else if (format == GBM_FORMAT_XRGB8888) {
        return WL_SHM_FORMAT_XRGB8888;
    } else {
        return (wl_shm_format)format;
    }
}

static void frame_handle_linux_dmabuf(void *, struct zwlr_screencopy_frame_v1 *frame,
    uint32_t format, uint32_t width, uint32_t height)
{
    if (!use_dmabuf) {
        return;
    }

    auto& buffer = buffers.capture();

    auto old_format = buffer.format;
    buffer.format = drm_to_wl_shm_format(format);
    buffer.drm_format = format;
    buffer.width = width;
    buffer.height = height;

    if (!buffer.wl_buffer || (old_format != buffer.format)) {
        if (buffer.bo) {
            if (buffer.wl_buffer) {
                wl_buffer_destroy(buffer.wl_buffer);
            }

            zwp_linux_buffer_params_v1_destroy(buffer.params);
            gbm_bo_destroy(buffer.bo);
        }

        const uint64_t modifier = 0; // DRM_FORMAT_MOD_LINEAR
        buffer.bo = gbm_bo_create_with_modifiers(gbm_device, buffer.width,
            buffer.height, format, &modifier, 1);
        if (buffer.bo == NULL)
        {
            buffer.bo = gbm_bo_create(gbm_device, buffer.width,
                buffer.height, format, GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);
        }
        if (buffer.bo == NULL)
        {
            std::cerr << "Failed to create gbm bo" << std::endl;
            exit_main_loop = true;
            return;
        }

        buffer.stride = gbm_bo_get_stride(buffer.bo);

        buffer.params = zwp_linux_dmabuf_v1_create_params(dmabuf);

        uint64_t mod = gbm_bo_get_modifier(buffer.bo);
        zwp_linux_buffer_params_v1_add(buffer.params,
            gbm_bo_get_fd(buffer.bo), 0,
            gbm_bo_get_offset(buffer.bo, 0),
            gbm_bo_get_stride(buffer.bo),
            mod >> 32, mod & 0xffffffff);

        zwp_linux_buffer_params_v1_add_listener(buffer.params, &params_listener, frame);
        zwp_linux_buffer_params_v1_create(buffer.params, buffer.width,
            buffer.height, format, 0);
    } else {
        if (use_damage) {
            zwlr_screencopy_frame_v1_copy_with_damage(frame, buffer.wl_buffer);
        } else {
            zwlr_screencopy_frame_v1_copy(frame, buffer.wl_buffer);
        }
    }
}

static void frame_handle_buffer_done(void *, struct zwlr_screencopy_frame_v1 *) {
}

static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
    .buffer = frame_handle_buffer,
    .flags = frame_handle_flags,
    .ready = frame_handle_ready,
    .failed = frame_handle_failed,
    .damage = frame_handle_damage,
    .linux_dmabuf = frame_handle_linux_dmabuf,
    .buffer_done = frame_handle_buffer_done,
};

static void dmabuf_feedback_done(void *, struct zwp_linux_dmabuf_feedback_v1 *feedback)
{
    zwp_linux_dmabuf_feedback_v1_destroy(feedback);
}

static void dmabuf_feedback_format_table(void *, struct zwp_linux_dmabuf_feedback_v1 *,
    int32_t fd, uint32_t)
{
    close(fd);
}

static void dmabuf_feedback_main_device(void *, struct zwp_linux_dmabuf_feedback_v1 *,
    struct wl_array *device)
{
    dev_t dev_id;
    memcpy(&dev_id, device->data, device->size);

    drmDevice *dev = NULL;
    if (drmGetDeviceFromDevId(dev_id, 0, &dev) != 0) {
        std::cerr << "Failed to get DRM device from dev id " << strerror(errno) << std::endl;
        return;
    }

    if (dev->available_nodes & (1 << DRM_NODE_RENDER)) {
        drm_device_name = dev->nodes[DRM_NODE_RENDER];
    } else if (dev->available_nodes & (1 << DRM_NODE_PRIMARY)) {
        drm_device_name = dev->nodes[DRM_NODE_PRIMARY];
    }

    drmFreeDevice(&dev);
}

static void dmabuf_feedback_tranche_done(void *, struct zwp_linux_dmabuf_feedback_v1 *)
{
}

static void dmabuf_feedback_tranche_target_device(void *, struct zwp_linux_dmabuf_feedback_v1 *,
    struct wl_array *)
{
}

static void dmabuf_feedback_tranche_formats(void *, struct zwp_linux_dmabuf_feedback_v1 *,
    struct wl_array *)
{
}

static void dmabuf_feedback_tranche_flags(void *, struct zwp_linux_dmabuf_feedback_v1 *,
    uint32_t)
{
}

static const struct zwp_linux_dmabuf_feedback_v1_listener dmabuf_feedback_listener = {
    .done = dmabuf_feedback_done,
    .format_table = dmabuf_feedback_format_table,
    .main_device = dmabuf_feedback_main_device,
    .tranche_done = dmabuf_feedback_tranche_done,
    .tranche_target_device = dmabuf_feedback_tranche_target_device,
    .tranche_formats = dmabuf_feedback_tranche_formats,
    .tranche_flags = dmabuf_feedback_tranche_flags,
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
            &zwlr_screencopy_manager_v1_interface, 3);
    }
    else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0)
    {
        xdg_output_manager = (zxdg_output_manager_v1*) wl_registry_bind(registry, name,
            &zxdg_output_manager_v1_interface, 2); // version 2 for name & description, if available
    }
    else if (strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0)
    {
        dmabuf = (zwp_linux_dmabuf_v1*) wl_registry_bind(registry, name,
            &zwp_linux_dmabuf_v1_interface, 4);
        if (dmabuf) {
            struct zwp_linux_dmabuf_feedback_v1 *feedback =
                zwp_linux_dmabuf_v1_get_default_feedback(dmabuf);
            zwp_linux_dmabuf_feedback_v1_add_listener(feedback, &dmabuf_feedback_listener, NULL);
        }
    }
}

static void handle_global_remove(void*, struct wl_registry *, uint32_t) {
    // Who cares?
}

static const struct wl_registry_listener registry_listener = {
    .global = handle_global,
    .global_remove = handle_global_remove,
};

static uint64_t timespec_to_usec (const timespec& ts)
{
    return ts.tv_sec * 1000000ll + 1ll * ts.tv_nsec / 1000ll;
}

static InputFormat get_input_format(wf_buffer& buffer)
{
    if (use_dmabuf && !use_hwupload) {
        return INPUT_FORMAT_DMABUF;
    }
    switch (buffer.format) {
    case WL_SHM_FORMAT_ARGB8888:
    case WL_SHM_FORMAT_XRGB8888:
        return INPUT_FORMAT_BGR0;
    case WL_SHM_FORMAT_XBGR8888:
    case WL_SHM_FORMAT_ABGR8888:
        return INPUT_FORMAT_RGB0;
    case WL_SHM_FORMAT_BGR888:
        return INPUT_FORMAT_BGR8;
    case WL_SHM_FORMAT_RGB565:
        return INPUT_FORMAT_RGB565;
    case WL_SHM_FORMAT_BGR565:
        return INPUT_FORMAT_BGR565;
    case WL_SHM_FORMAT_ARGB2101010:
    case WL_SHM_FORMAT_XRGB2101010:
        return INPUT_FORMAT_X2RGB10;
    case WL_SHM_FORMAT_ABGR2101010:
    case WL_SHM_FORMAT_XBGR2101010:
        return INPUT_FORMAT_X2BGR10;
    case WL_SHM_FORMAT_ABGR16161616:
    case WL_SHM_FORMAT_XBGR16161616:
        return INPUT_FORMAT_RGBX64;
    case WL_SHM_FORMAT_ARGB16161616:
    case WL_SHM_FORMAT_XRGB16161616:
        return INPUT_FORMAT_BGRX64;
    case WL_SHM_FORMAT_ABGR16161616F:
    case WL_SHM_FORMAT_XBGR16161616F:
        return INPUT_FORMAT_RGBX64F;
    default:
        fprintf(stderr, "Unsupported buffer format %d, exiting.", buffer.format);
        std::exit(0);
    }
}

static void write_loop(FrameWriterParams params)
{
    /* Ignore SIGTERM/SIGINT/SIGHUP, main loop is responsible for the exit_main_loop signal */
    sigset_t sigset;
    sigemptyset(&sigset);
    for (auto signo : GRACEFUL_TERMINATION_SIGNALS)
    {
        sigaddset(&sigset, signo);
    }
    pthread_sigmask(SIG_BLOCK, &sigset, NULL);

#ifdef HAVE_AUDIO
    std::unique_ptr<AudioReader> pr;
#endif

    std::optional<uint64_t> first_frame_ts;

    while(!exit_main_loop)
    {
        // wait for frame to become available
        while(buffers.encode().ready_encode() != true && !exit_main_loop) {
            std::this_thread::sleep_for(std::chrono::microseconds(1000));
        }
        if (exit_main_loop) {
            break;
        }

        auto& buffer = buffers.encode();

        frame_writer_pending_mutex.lock();
        frame_writer_mutex.lock();
        frame_writer_pending_mutex.unlock();

        if (!frame_writer)
        {
            /* This is the first time buffer attributes are available */
            params.format = get_input_format(buffer);
            params.drm_format = buffer.drm_format;
            params.width = buffer.width;
            params.height = buffer.height;
            params.stride = buffer.stride;
            frame_writer = std::unique_ptr<FrameWriter> (new FrameWriter(params));

#ifdef HAVE_AUDIO
            if (params.enable_audio)
            {
                audioParams.audio_frame_size = frame_writer->get_audio_buffer_size();
                audioParams.sample_rate = params.sample_rate;
                pr = std::unique_ptr<AudioReader> (AudioReader::create(audioParams));
                if (pr)
                {
                    pr->start();
                }
            }
#endif
        }

        bool drop = false;
        uint64_t sync_timestamp = 0;
        if (first_frame_ts.has_value()) {
            sync_timestamp = buffer.base_usec - first_frame_ts.value();
        } else if (pr) {
            if (!pr->get_time_base() || pr->get_time_base() > buffer.base_usec) {
                drop = true;
            } else {
                first_frame_ts = pr->get_time_base();
                sync_timestamp = buffer.base_usec - first_frame_ts.value();
            }
        } else {
            sync_timestamp = 0;
            first_frame_ts = buffer.base_usec;
        }

        bool do_cont = false;

        if (!drop) {
            if (use_dmabuf) {
                if (use_hwupload) {
                    uint32_t stride = 0;
                    void *map_data = NULL;
                    void *data = gbm_bo_map(buffer.bo, 0, 0, buffer.width, buffer.height,
                        GBM_BO_TRANSFER_READ, &stride, &map_data);
                    if (!data) {
                        std::cerr << "Failed to map bo" << std::endl;
                        break;
                    }
                    do_cont = frame_writer->add_frame((unsigned char*)data,
                        sync_timestamp, buffer.y_invert);
                    gbm_bo_unmap(buffer.bo, map_data);
                } else {
                    do_cont = frame_writer->add_frame(buffer.bo,
                        sync_timestamp, buffer.y_invert);
                }
            } else {
                do_cont = frame_writer->add_frame((unsigned char*)buffer.data,
                    sync_timestamp, buffer.y_invert);
            }
        } else {
            do_cont = true;
        }

        frame_writer_mutex.unlock();

        if (!do_cont) {
            break;
        }

        buffers.next_encode();
    }

    std::lock_guard<std::mutex> lock(frame_writer_mutex);
    /* Free the AudioReader connection first. This way it'd flush any remaining
     * frames to the FrameWriter */
#ifdef HAVE_AUDIO
    pr = nullptr;
#endif
    frame_writer = nullptr;
}

void handle_graceful_termination(int)
{
    exit_main_loop = true;
}

static bool user_specified_overwrite(std::string filename)
{
    struct stat buffer;   
    if (stat (filename.c_str(), &buffer) == 0 && !S_ISCHR(buffer.st_mode))
    {
        std::string input;
        std::cerr << "Output file \"" << filename << "\" exists. Overwrite? Y/n: ";
        std::getline(std::cin, input);
        if (input.size() && input[0] != 'Y' && input[0] != 'y')
        {
            std::cerr << "Use -f to specify the file name." << std::endl;
            return false;
	}
    }

    return true;
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

    if (use_dmabuf && dmabuf == NULL) {
        fprintf(stderr, "compositor doesn't support linux-dmabuf-unstable-v1\n");
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

static wf_recorder_output* choose_interactive()
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
        return nullptr;

    auto it = available_outputs.begin();
    std::advance(it, choice - 1);
    return &*it;
}

struct capture_region
{
    int32_t x, y;
    int32_t width, height;

    capture_region()
        : capture_region(0, 0, 0, 0) {}

    capture_region(int32_t _x, int32_t _y, int32_t _width, int32_t _height)
        : x(_x), y(_y), width(_width), height(_height) { }

    void set_from_string(std::string geometry_string)
    {
        if (sscanf(geometry_string.c_str(), "%d,%d %dx%d", &x, &y, &width, &height) != 4)
        {
            fprintf(stderr, "Bad geometry: %s, capturing whole output instead.\n",
                geometry_string.c_str());
            x = y = width = height = 0;
            return;
        }
    }

    bool is_selected()
    {
        return width > 0 && height > 0;
    }

    bool contained_in(const capture_region& output) const
    {
        return
            output.x <= x &&
            output.x + output.width >= x + width &&
            output.y <= y &&
            output.y + output.height >= y + height;
    }
};

static wf_recorder_output* detect_output_from_region(const capture_region& region)
{
    for (auto& wo : available_outputs)
    {
        const capture_region output_region{wo.x, wo.y, wo.width, wo.height};
        if (region.contained_in(output_region))
        {
            std::cerr << "Detected output based on geometry: " << wo.name << std::endl;
            return &wo;
        }
    }

    std::cerr << "Failed to detect output based on geometry (is your geometry overlapping outputs?)" << std::endl;
    return nullptr;
}

static void help()
{
    printf(R"(Usage: wf-recorder [OPTION]... -f [FILE]...
Screen recording of wlroots-based compositors

With no FILE, start recording the current screen.

Use Ctrl+C to stop.)");
#ifdef HAVE_AUDIO
    printf(R"(

  -a, --audio[=DEVICE]      Starts recording the screen with audio.
                            [=DEVICE] argument is optional.
                            In case you want to specify the audio device which will capture
                            the audio, you can run this command with the name of that device.
                            You can find your device by running: pactl list sources | grep Name
                            Specify device like this: -a<device> or --audio=<device>)");
#endif
    printf(R"(

  -c, --codec               Specifies the codec of the video. These can be found by using:
                            ffmpeg -encoders
                            To modify codec parameters, use -p <option_name>=<option_value>
  
  -r, --framerate           Changes framerate to constant framerate with a given value.
  
  -d, --device              Selects the device to use when encoding the video
                            Some drivers report support for rgb0 data for vaapi input but
                            really only support yuv.

  --no-dmabuf               By default, wf-recorder will try to use only GPU buffers and copies if
                            using a GPU encoder. However, this can cause issues on some systems. In such
                            cases, this option will disable the GPU copy and force a CPU one.

  -D, --no-damage           By default, wf-recorder will request a new frame from the compositor
                            only when the screen updates. This results in a much smaller output
                            file, which however has a variable refresh rate. When this option is
                            on, wf-recorder does not use this optimization and continuously
                            records new frames, even if there are no updates on the screen.

  -f <filename>.ext         By using the -f option the output file will have the name :
                            filename.ext and the file format will be determined by provided
                            while extension .ext . If the extension .ext provided is not
                            recognized by your FFmpeg muxers, the command will fail.
                            You can check the muxers that your FFmpeg installation supports by
                            running: ffmpeg -muxers

  -m, --muxer               Set the output format to a specific muxer instead of detecting it
                            from the filename.

  -x, --pixel-format        Set the output pixel format. These can be found by running:
                            ffmpeg -pix_fmts

  -g, --geometry            Selects a specific part of the screen. The format is "x,y WxH".

  -h, --help                Prints this help screen.

  -v, --version             Prints the version of wf-recorder.

  -l, --log                 Generates a log on the current terminal. Debug purposes.

  -o, --output              Specify the output where the video is to be recorded.

  -p, --codec-param         Change the codec parameters.
                            -p <option_name>=<option_value>

  -F, --filter              Specify the ffmpeg filter string to use. For example,
                            -F scale_vaapi=format=nv12 is used for VAAPI.

  -b, --bframes             This option is used to set the maximum number of b-frames to be used.
                            If b-frames are not supported by your hardware, set this to 0.
    
  -B. --buffrate            This option is used to specify the buffers expected framerate. this 
                            may help when encoders are expecting specific or limited framerate.

  --audio-backend           Specifies the audio backend among the available backends, for ex.
                            --audio-backend=pipewire
  
  -C, --audio-codec         Specifies the codec of the audio. These can be found by running:
                            ffmpeg -encoders
                            To modify codec parameters, use -P <option_name>=<option_value>

  -X, --sample-format       Set the output audio sample format. These can be found by running: 
                            ffmpeg -sample_fmts
  
  -R, --sample-rate         Changes the audio sample rate in HZ. The default value is 48000.
  
  -P, --audio-codec-param   Change the audio codec parameters.
                            -P <option_name>=<option_value>
  
  -y, --overwrite           Force overwriting the output file without prompting.

Examples:)");
#ifdef HAVE_AUDIO
    printf(R"(

  Video Only:)");
#endif
    printf(R"(

  - wf-recorder                         Records the video. Use Ctrl+C to stop recording.
                                        The video file will be stored as recording.mp4 in the
                                        current working directory.

  - wf-recorder -f <filename>.ext       Records the video. Use Ctrl+C to stop recording.
                                        The video file will be stored as <filename>.ext in the
                                        current working directory.)");
#ifdef HAVE_AUDIO
    printf(R"(

  Video and Audio:

  - wf-recorder -a                      Records the video and audio. Use Ctrl+C to stop recording.
                                        The video file will be stored as recording.mp4 in the
                                        current working directory.

  - wf-recorder -a -f <filename>.ext    Records the video and audio. Use Ctrl+C to stop recording.
                                        The video file will be stored as <filename>.ext in the
                                        current working directory.)");
#endif
    printf(R"(

)" "\n");
    exit(EXIT_SUCCESS);
}

capture_region selected_region{};
wf_recorder_output *chosen_output = nullptr;
zwlr_screencopy_frame_v1 *frame = NULL;

void request_next_frame()
{
    if (frame != NULL)
    {
        zwlr_screencopy_frame_v1_destroy(frame);
    }

    /* Capture the whole output if the user hasn't provided a good geometry */
    if (!selected_region.is_selected())
    {
        frame = zwlr_screencopy_manager_v1_capture_output(
            screencopy_manager, 1, chosen_output->output);
    } else
    {
        frame = zwlr_screencopy_manager_v1_capture_output_region(
            screencopy_manager, 1, chosen_output->output,
            selected_region.x - chosen_output->x,
            selected_region.y - chosen_output->y,
            selected_region.width, selected_region.height);
    }

    zwlr_screencopy_frame_v1_add_listener(frame, &frame_listener, NULL);
}

static void parse_codec_opts(std::map<std::string, std::string>& options, const std::string param)
{
    size_t pos;
    pos = param.find("=");
    if (pos != std::string::npos && pos != param.length() -1)
    {
        auto optname = param.substr(0, pos);
        auto optvalue = param.substr(pos + 1, param.length() - pos - 1);
        options.insert(std::pair<std::string, std::string>(optname, optvalue));
    } else
    {
        std::cerr << "Invalid codec option " + param << std::endl;
    }
}

int main(int argc, char *argv[])
{
    FrameWriterParams params = FrameWriterParams(exit_main_loop);
    params.file = "recording." + std::string(DEFAULT_CONTAINER_FORMAT);
    params.codec = DEFAULT_CODEC;
    params.pix_fmt = DEFAULT_PIX_FMT;
    params.audio_codec = DEFAULT_AUDIO_CODEC;
    params.sample_rate = DEFAULT_AUDIO_SAMPLE_RATE;
    params.enable_ffmpeg_debug_output = false;
    params.enable_audio = false;
    params.bframes = -1;

    constexpr const char* default_cmdline_output = "interactive";
    std::string cmdline_output = default_cmdline_output;
    bool force_no_dmabuf = false;
    bool force_overwrite = false;

    struct option opts[] = {
        { "output",            required_argument, NULL, 'o' },
        { "file",              required_argument, NULL, 'f' },
        { "muxer",             required_argument, NULL, 'm' },
        { "geometry",          required_argument, NULL, 'g' },
        { "codec",             required_argument, NULL, 'c' },
        { "codec-param",       required_argument, NULL, 'p' },
        { "framerate",         required_argument, NULL, 'r' },
        { "pixel-format",      required_argument, NULL, 'x' },
        { "audio-backend",     required_argument, NULL, '*' },
        { "audio-codec",       required_argument, NULL, 'C' },
        { "audio-codec-param", required_argument, NULL, 'P' },
        { "sample-rate",       required_argument, NULL, 'R' },
        { "sample-format",     required_argument, NULL, 'X' },
        { "device",            required_argument, NULL, 'd' },
        { "no-dmabuf",         no_argument,       NULL, '&' },
        { "filter",            required_argument, NULL, 'F' },
        { "log",               no_argument,       NULL, 'l' },
        { "audio",             optional_argument, NULL, 'a' },
        { "help",              no_argument,       NULL, 'h' },
        { "bframes",           required_argument, NULL, 'b' },
        { "buffrate",          required_argument, NULL, 'B' },
        { "version",           no_argument,       NULL, 'v' },
        { "no-damage",         no_argument,       NULL, 'D' },
        { "overwrite",         no_argument,       NULL, 'y' },
        { 0,                   0,                 NULL,  0  }
    };

    int c, i;
    while((c = getopt_long(argc, argv, "o:f:m:g:c:p:r:x:C:P:R:X:d:b:B:la::hvDF:y", opts, &i)) != -1)
    {
        switch(c)
        {
            case 'f':
                params.file = optarg;
                break;

            case 'F':
                params.video_filter = optarg;
                break;

            case 'o':
                cmdline_output = optarg;
                break;

            case 'm':
                params.muxer = optarg;
                break;

            case 'g':
                selected_region.set_from_string(optarg);
                break;

            case 'c':
                params.codec = optarg;
                break;

            case 'r':
                params.framerate = atoi(optarg);
                break;

            case 'x':
                params.pix_fmt = optarg;
                break;

            case 'C':
                params.audio_codec = optarg;
                break;

            case 'R':
                params.sample_rate = atoi(optarg);
                break;

            case 'X':
                params.sample_fmt = optarg;
                break;

            case 'd':
                params.hw_device = optarg;
                break;

            case 'b':
                params.bframes = atoi(optarg);
                break;

            case 'B':
                params.buffrate = atoi(optarg);
                break;

            case 'l':
                params.enable_ffmpeg_debug_output = true;
                break;

            case 'a':
#ifdef HAVE_AUDIO
                params.enable_audio = true;
                audioParams.audio_source = optarg ? strdup(optarg) : NULL;
#else
                std::cerr << "Cannot record audio. Built without audio support." << std::endl;
                return EXIT_FAILURE;
#endif
                break;

            case 'h':
                help();
                break;

            case 'p':
                parse_codec_opts(params.codec_options, optarg);
                break;

            case 'v':
                printf("wf-recorder %s\n", WFRECORDER_VERSION);
                return 0;

            case 'D':
                use_damage = false;
                break;

            case 'P':
                parse_codec_opts(params.audio_codec_options, optarg);
                break;

            case '&':
                force_no_dmabuf = true;
                break;

            case 'y':
                force_overwrite = true;
                break;

            case '*':
                audioParams.audio_backend = optarg;
                break;

            default:
                printf("Unsupported command line argument %s\n", optarg);
        }
    }

    if (!force_overwrite && !user_specified_overwrite(params.file))
    {
        return EXIT_FAILURE;
    }

    display = wl_display_connect(NULL);
    if (display == NULL)
    {
        fprintf(stderr, "failed to create display: %m\n");
        return EXIT_FAILURE;
    }

    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    sync_wayland();

    if (params.codec.find("vaapi") != std::string::npos)
    {
        std::cerr << "using VA-API, trying to enable DMA-BUF capture..." << std::endl;

        // try compositor device if not explicitly set
        if (params.hw_device.empty())
        {
            params.hw_device = drm_device_name;
        }

        // check we use same device as compositor
        if (!params.hw_device.empty() && params.hw_device == drm_device_name && !force_no_dmabuf)
        {
            use_dmabuf = true;
        } else if (force_no_dmabuf) {
            std::cerr << "Disabling DMA-BUF as requested on command line" << std::endl;
        } else {
            std::cerr << "compositor running on different device, disabling DMA-BUF" << std::endl;
        }

        // region with dmabuf needs wlroots >= 0.17
        if (use_dmabuf && selected_region.is_selected())
        {
            std::cerr << "region capture may not work with older wlroots, try --no-dmabuf if it fails" << std::endl;
        }

        if (params.video_filter == "null")
        {
            params.video_filter = "scale_vaapi=format=nv12:out_range=full";
            if (!use_dmabuf)
            {
                params.video_filter.insert(0, "hwupload,");
            }
        }

        if (use_dmabuf)
        {
            std::cerr << "enabled DMA-BUF capture, device " << params.hw_device.c_str() << std::endl;

            drm_fd = open(params.hw_device.c_str(), O_RDWR);
            if (drm_fd < 0)
            {
                fprintf(stderr, "failed to open drm device: %m\n");
                return EXIT_FAILURE;
            }

            gbm_device = gbm_create_device(drm_fd);
            if (gbm_device == NULL)
            {
                fprintf(stderr, "failed to create gbm device: %m\n");
                return EXIT_FAILURE;
            }

            use_hwupload = params.video_filter.find("hwupload") != std::string::npos;
        }
    }

    check_has_protos();
    load_output_info();

    if (available_outputs.size() == 1)
    {
        chosen_output = &available_outputs.front();
        if (chosen_output->name != cmdline_output &&
            cmdline_output != default_cmdline_output)
        {
            std::cerr << "Couldn't find requested output "
                << cmdline_output << std::endl;
            return EXIT_FAILURE;
        }
    } else
    {
        for (auto& wo : available_outputs)
        {
            if (wo.name == cmdline_output)
                chosen_output = &wo;
        }

        if (chosen_output == NULL)
        {
            if (cmdline_output != default_cmdline_output)
            {
                std::cerr << "Couldn't find requested output "
                    << cmdline_output.c_str() << std::endl;
                return EXIT_FAILURE;
            }

            if (selected_region.is_selected())
            {
                chosen_output = detect_output_from_region(selected_region);
            }
            else
            {
                chosen_output = choose_interactive();
            }
        }
    }


    if (chosen_output == nullptr)
    {
        fprintf(stderr, "Failed to select output, exiting\n");
        return EXIT_FAILURE;
    }

    if (selected_region.is_selected())
    {
        if (!selected_region.contained_in({chosen_output->x, chosen_output->y,
            chosen_output->width, chosen_output->height}))
        {
            fprintf(stderr, "Invalid region to capture: must be completely "
                "inside the output\n");
            selected_region = capture_region{};
        }
    }

    printf("selected region %d,%d %dx%d\n", selected_region.x, selected_region.y, selected_region.width, selected_region.height);

    bool spawned_thread = false;
    std::thread writer_thread;

    for (auto signo : GRACEFUL_TERMINATION_SIGNALS)
    {
        signal(signo, handle_graceful_termination);
    }

    while(!exit_main_loop)
    {
        // wait for a free buffer
        while(buffers.capture().ready_capture() != true) {
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }

        buffer_copy_done = false;
        request_next_frame();

        while (!buffer_copy_done && !exit_main_loop && wl_display_dispatch(display) != -1) {
            // This space is intentionally left blank
        }

        if (exit_main_loop) {
            break;
        }

        auto& buffer = buffers.capture();
        //std::cout << "first buffer at " << timespec_to_usec(get_ct()) / 1.0e6<< std::endl;

        if (!spawned_thread)
        {
            writer_thread = std::thread([=] () {
                write_loop(params);
            });

            spawned_thread = true;
        }

        buffer.base_usec = timespec_to_usec(buffer.presented);
        buffers.next_capture();
    }

    if (writer_thread.joinable())
    {
        writer_thread.join();
    }

    for (size_t i = 0; i < buffers.size(); ++i)
    {
        auto buffer = buffers.at(i);
        if (buffer && buffer->wl_buffer)
            wl_buffer_destroy(buffer->wl_buffer);
    }

    if (gbm_device) {
        gbm_device_destroy(gbm_device);
        close(drm_fd);
    }

    return EXIT_SUCCESS;
}
