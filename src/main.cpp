#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 199309L
#include <iostream>

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
#include <sys/stat.h>
#include <signal.h>
#include <unistd.h>
#include <wayland-client-protocol.h>

#include "frame-writer.hpp"
#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"

#include "config.h"

#ifdef HAVE_OPENCL
std::unique_ptr<OpenCL> opencl;
#endif

#ifdef HAVE_PULSE
#include "pulse.hpp"
PulseReaderParams pulseParams;
#endif

std::mutex frame_writer_mutex, frame_writer_pending_mutex;
std::unique_ptr<FrameWriter> frame_writer;

static struct wl_shm *shm = NULL;
static struct zxdg_output_manager_v1 *xdg_output_manager = NULL;
static struct zwlr_screencopy_manager_v1 *screencopy_manager = NULL;

struct wf_recorder_output
{
    wl_output *output;
    zxdg_output_v1 *zxdg_output;
    std::string name, description;
    int32_t x, y, width, height;
};

std::vector<wf_recorder_output> available_outputs;

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

struct wf_buffer
{
    struct wl_buffer *wl_buffer;
    void *data;
    enum wl_shm_format format;
    int width, height, stride;
    bool y_invert;

    timespec presented;
    uint64_t base_usec;

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

    /* ffmpeg requires even width and height */
    if (buffer.width % 2)
        buffer.width -= 1;
    if (buffer.height % 2)
        buffer.height -= 1;

    if (!buffer.wl_buffer) {
        buffer.wl_buffer =
            create_shm_buffer(format, width, height, stride, &buffer.data);
    }

    if (buffer.wl_buffer == NULL) {
        fprintf(stderr, "failed to create buffer\n");
        exit(EXIT_FAILURE);
    }

    zwlr_screencopy_frame_v1_copy_with_damage(frame, buffer.wl_buffer);
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
    exit_main_loop = true;
}

static void frame_handle_damage(void *, struct zwlr_screencopy_frame_v1 *,
    uint32_t, uint32_t, uint32_t, uint32_t)
{
}

static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
    .buffer = frame_handle_buffer,
    .flags = frame_handle_flags,
    .ready = frame_handle_ready,
    .failed = frame_handle_failed,
    .damage = frame_handle_damage,
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
            &zwlr_screencopy_manager_v1_interface, 2);
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

static uint64_t timespec_to_usec (const timespec& ts)
{
    return ts.tv_sec * 1000000ll + 1ll * ts.tv_nsec / 1000ll;
}

static int next_frame(int frame)
{
    return (frame + 1) % MAX_BUFFERS;
}

static InputFormat get_input_format(wf_buffer& buffer)
{
    if (buffer.format == WL_SHM_FORMAT_ARGB8888)
        return INPUT_FORMAT_BGR0;
    if (buffer.format == WL_SHM_FORMAT_XRGB8888)
        return INPUT_FORMAT_BGR0;

    if (buffer.format == WL_SHM_FORMAT_XBGR8888)
        return INPUT_FORMAT_RGB0;
    if (buffer.format == WL_SHM_FORMAT_ABGR8888)
        return INPUT_FORMAT_RGB0;

    fprintf(stderr, "Unsupported buffer format %d, exiting.", buffer.format);
    std::exit(0);
}

static void write_loop(FrameWriterParams params)
{
    /* Ignore SIGINT, main loop is responsible for the exit_main_loop signal */
    sigset_t sigset;
    sigemptyset(&sigset);
    sigaddset(&sigset, SIGINT);
    pthread_sigmask(SIG_BLOCK, &sigset, NULL);

    int last_encoded_frame = 0;
#ifdef HAVE_PULSE
    std::unique_ptr<PulseReader> pr;
#endif

    while(!exit_main_loop)
    {
        // wait for frame to become available
        while(buffers[last_encoded_frame].available != true) {
            std::this_thread::sleep_for(std::chrono::microseconds(1000));
        }
        auto& buffer = buffers[last_encoded_frame];

        frame_writer_pending_mutex.lock();
        frame_writer_mutex.lock();
        frame_writer_pending_mutex.unlock();

        if (!frame_writer)
        {
            /* This is the first time buffer attributes are available */
            params.format = get_input_format(buffer);
            params.width = buffer.width;
            params.height = buffer.height;
            params.stride = buffer.stride;
            frame_writer = std::unique_ptr<FrameWriter> (new FrameWriter(params));

#ifdef HAVE_OPENCL
            if (params.opencl && params.force_yuv)
            {
                frame_writer->opencl = std::move(opencl);
                frame_writer->opencl->init(params.width, params.height);
            }
#endif

#ifdef HAVE_PULSE
            if (params.enable_audio)
            {
                pulseParams.audio_frame_size = frame_writer->get_audio_buffer_size();
                pr = std::unique_ptr<PulseReader> (new PulseReader(pulseParams));
                pr->start();
            }
#endif
        }

        frame_writer->add_frame((unsigned char*)buffer.data, buffer.base_usec,
            buffer.y_invert);

        frame_writer_mutex.unlock();

        buffer.available = false;
        buffer.released = true;

        last_encoded_frame = next_frame(last_encoded_frame);
    }

    std::lock_guard<std::mutex> lock(frame_writer_mutex);
    /* Free the PulseReader connection first. This way it'd flush any remaining
     * frames to the FrameWriter */
#ifdef HAVE_PULSE
    pr = nullptr;
#endif
    frame_writer = nullptr;
}

void handle_sigint(int)
{
    exit_main_loop = true;
}

static bool user_specified_overwrite(std::string filename)
{
    struct stat buffer;   
    if (stat (filename.c_str(), &buffer) == 0)
    {
        std::string input;
        std::cout << "Output file \"" << filename << "\" exists. Overwrite? Y/n: ";
        std::getline(std::cin, input);
        if (input.size() && input[0] != 'Y' && input[0] != 'y')
        {
            std::cout << "Use -f to specify the file name." << std::endl;
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

    return &available_outputs[choice - 1];
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
            std::cout << "Detected output based on geometry: " << wo.name << std::endl;
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
#ifdef HAVE_PULSE
    printf(R"(

  -a, --audio [DEVICE]      Starts recording the screen with audio.
                            [DEVICE] argument is optional.
                            In case you want to specify the pulseaudio device which will capture
                            the audio, you can run this command with the name of that device.
                            You can find your device by running: pactl list sinks | grep Name)");
#endif
    printf(R"(

  -c, --codec               Specifies the codec of the video. Supports  GIF output also.
                            To modify codec parameters, use -p <option_name>=<option_value>

  -d, --device              Selects the device to use when encoding the video
                            Some drivers report support for rgb0 data for vaapi input but
                            really only support yuv.

  -f <filename>.ext         By using the -f option the output file will have the name :
                            filename.ext and the file format will be determined by provided
                            while extension .ext . If the extension .ext provided is not
                            recognized by your FFmpeg muxers, the command will fail.
                            You can check the muxers that your FFmpeg installation supports by
                            running  : ffmpeg -muxers

  -m, --muxer               Set the output format to a specific muxer instead of detecting it
                            from the filename.

  -x, --pixel-format        Set the output pixel format. These can be found by running:
                            *ffmpeg -pix_fmts*

  -g, --geometry            Selects a specific part of the screen.

  -h, --help                Prints this help screen.

  -l, --log                 Generates a log on the current terminal. Debug purposes.

  -o, --output              Specify the output where the video is to be recorded.

  -p, --codec-param         Change the codec parameters.
                            -p <option_name>=<option_value>)");
#ifdef HAVE_OPENCL
    printf(R"(

  -e, --opencl              Use the -e[#] or --opencl[=#] in conjunction with -t or --force-yuv option
                            to use opencl for gpu accelerated conversion of data to yuv. # is one
                            of the devices listed when running without specifying #.)");
#endif
    printf(R"(

  -t, --force-yuv           Use the -t or --force-yuv option to force conversion of the data to
                            yuv format, before sending it to the gpu.

  -b, --bframes             This option is used to set the maximum number of b-frames to be used.
                            If b-frames are not supported by your hardware, set this to 0.)" "\n\n" R"(
Examples:)");
#ifdef HAVE_PULSE
    printf(R"(

  Video Only:)");
#endif
    printf(R"(

  - wf-recorder                         Records the video. Use Ctrl+C to stop recording.
                                        The video file will be stored as recording.mp4 in the
                                        current working directory.

  - wf-recorder -f <filename>.ext       Records the video. Use Ctrl+C to stop recording.
                                        The video file will be stored as <outputname>.ext in the
                                        current working directory.)");
#ifdef HAVE_PULSE
    printf(R"(

  Video and Audio:

  - wf-recorder -a                      Records the audio. Use Ctrl+C to stop recording.
                                        The video file will be stored as recording.mp4 in the
                                        current working directory.

  - wf-recorder -a -f <filename>.ext    Records the audio. Use Ctrl+C to stop recording.
                                        The video file will be stored as <outputname>.ext in the
                                        current working directory.)");
#endif
    printf(R"(

)" "\n");
    exit(EXIT_SUCCESS);
}

int main(int argc, char *argv[])
{
    FrameWriterParams params = FrameWriterParams(exit_main_loop);
    params.file = "recording.mp4";
    params.codec = DEFAULT_CODEC;
    params.enable_ffmpeg_debug_output = false;
    params.enable_audio = false;
    params.force_yuv = false;
    params.opencl = false;
    params.opencl_device = -1;
    params.bframes = -1;

    constexpr const char* default_cmdline_output = "interactive";
    std::string cmdline_output = default_cmdline_output;

    capture_region selected_region{};

    struct option opts[] = {
        { "output",          required_argument, NULL, 'o' },
        { "file",            required_argument, NULL, 'f' },
        { "muxer",           required_argument, NULL, 'm' },
        { "pixel-format",    required_argument, NULL, 'x' },
        { "geometry",        required_argument, NULL, 'g' },
        { "codec",           required_argument, NULL, 'c' },
        { "codec-param",     required_argument, NULL, 'p' },
        { "device",          required_argument, NULL, 'd' },
        { "log",             no_argument,       NULL, 'l' },
        { "audio",           optional_argument, NULL, 'a' },
        { "help",            no_argument,       NULL, 'h' },
        { "force-yuv",       no_argument,       NULL, 't' },
        { "opencl",          optional_argument, NULL, 'e' },
        { "bframes",         required_argument, NULL, 'b' },
        { 0,                 0,                 NULL,  0  }
    };

    int c, i;
    std::string param;
    size_t pos;
    while((c = getopt_long(argc, argv, "o:f:m:x:g:c:p:d:b:la::te::h", opts, &i)) != -1)
    {
        switch(c)
        {
            case 'f':
                params.file = optarg;
                break;

            case 'o':
                cmdline_output = optarg;
                break;

            case 'm':
                params.muxer = optarg;
                break;

            case 'x':
                params.pix_fmt = optarg;
                break;

            case 'g':
                selected_region.set_from_string(optarg);
                break;

            case 'c':
                params.codec = optarg;
                break;

            case 'd':
                params.hw_device = optarg;
                break;

            case 'b':
                params.bframes = atoi(optarg);
                break;

            case 'l':
                params.enable_ffmpeg_debug_output = true;
                break;

            case 'a':
#ifdef HAVE_PULSE
                params.enable_audio = true;
                pulseParams.audio_source = optarg ? strdup(optarg) : NULL;
#else
                std::cerr << "Cannot record audio. Built without pulse support." << std::endl;
                return EXIT_FAILURE;
#endif
                break;

            case 't':
                params.force_yuv = true;
                break;

            case 'h':
                help();
                break;

            case 'e':
                params.opencl = true;
                params.opencl_device = optarg ? atoi(optarg) : -1;
                break;

            case 'p':
                param = optarg;
                pos = param.find("=");
                if (pos != std::string::npos && pos != param.length() - 1)
                {
                    auto optname = param.substr(0, pos);
                    auto optvalue = param.substr(pos + 1, param.length() - pos - 1);
                    params.codec_options[optname] = optvalue;
                } else
                {
                    printf("Invalid codec option %s\n", optarg);
                }
                break;

            default:
                printf("Unsupported command line argument %s\n", optarg);
        }
    }

    if (!user_specified_overwrite(params.file))
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

    check_has_protos();
    load_output_info();

    wf_recorder_output *chosen_output = nullptr;
    if (available_outputs.size() == 1)
    {
        chosen_output = &available_outputs[0];
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

    printf("selected region %d %d %d %d\n", selected_region.x, selected_region.y, selected_region.width, selected_region.height);

#ifdef HAVE_OPENCL
     if (params.opencl && params.force_yuv)
         opencl = std::unique_ptr<OpenCL> (new OpenCL(params.opencl_device));
#endif

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
        struct zwlr_screencopy_frame_v1 *frame = NULL;

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

        while (!buffer_copy_done && wl_display_dispatch(display) != -1) {
            // This space is intentionally left blank
        }

        auto& buffer = buffers[active_buffer];
        //std::cout << "first buffer at " << timespec_to_usec(get_ct()) / 1.0e6<< std::endl;

        if (!spawned_thread)
        {
            writer_thread = std::thread([=] () {
                write_loop(params);
            });

            spawned_thread = true;
        }

        if (first_frame.tv_sec == -1)
            first_frame = buffer.presented;

        buffer.base_usec = timespec_to_usec(buffer.presented)
            - timespec_to_usec(first_frame);

        buffer.released = false;
        buffer.available = true;

        active_buffer = next_frame(active_buffer);
        zwlr_screencopy_frame_v1_destroy(frame);
    }

    writer_thread.join();

    for (auto& buffer : buffers)
    {
        if (buffer.wl_buffer)
            wl_buffer_destroy(buffer.wl_buffer);
    }

    return EXIT_SUCCESS;
}
