# wf-recorder

wf-recorder is a utility program for screen recording of `wlroots`-based compositors (more specifically, those that support `wlr-screencopy-v1` and `xdg-output`). Its dependencies are `ffmpeg`, `wayland-client` and `wayland-protocols`.

# Installation

The following distributions are known to have packages:

[comment]: <> (List ordered alphabetically)

| Distribution | Installation                  |
| ------------ | ----------------------------- |
| Alpine       | ```apk add wf-recorder```     |
| Arch / Artix | ```pacman -S wf-recorder```   |
| Debian       | ```apt install wf-recorder``` |
| Fedora       | ```dnf install wf-recorder``` |
| Gentoo       | Available in the official `::gentoo` repository |
| NixOS / Nix  | Add `wf-recorder` to configuration or run any of: `nix-shell -p wf-recorder`, `nix shell nixpkgs#wfrecorder`, `nix run nixpkgs#wf-recorder` |
| Void         | ```xbps-install -S wf-recorder``` |

## From Source
### Install Dependencies

| Distribution | Install dependencies packages |
| ------------ | --------------------- |
| Ubuntu       | ```sudo apt install g++ meson libavutil-dev libavcodec-dev libavformat-dev libswscale-dev libpulse-dev``` |
| Fedora       | ```sudo dnf install gcc-c++ meson wayland-devel wayland-protocols-devel ffmpeg-free-devel pulseaudio-libs-devel``` |
| Void         | ```sudo xbps-install -S meson ninja gcc pkg-config scdoc wayland-devel wayland-protocols wayland-devel libgbm-devel libdrm-devel ffmpeg6-devel x264-devel pulseaudio-devel pipewire-devel``` |

### Download & Build
```
git clone https://github.com/ammen99/wf-recorder.git && cd wf-recorder
meson build --prefix=/usr --buildtype=release
ninja -C build
```
Optionally configure with `-Ddefault_codec='codec'`. The default is libx264. Now you can just run `./build/wf-recorder` or install it with `sudo ninja -C build install`.

The man page can be read with `man ./manpage/wf-recorder.1`.

# Usage
In its simplest form, run `wf-recorder` to start recording and use Ctrl+C to stop. This will create a file called `recording.mp4` in the current working directory using the default codec.

Use `-f <filename>` to specify the output file. In case of multiple outputs, you'll first be prompted to select the output you want to record. If you know the output name beforehand, you can use the `-o <output name>` option.
To view all available output options, use the list flag `-L` or `--list-output`

### Video cropping
To select a specific part of the screen you can either use `-g <geometry>`, or use [slurp](https://github.com/emersion/slurp) for interactive selection of the screen area that will be recorded:

```
wf-recorder -g "$(slurp)"
```

### Audio recording
You can record screen and sound simultaneously with

```
wf-recorder --audio --file=recording_with_audio.mp4
```

To specify an audio device, use the `-a<device>` or `--audio=<device>` options.

To specify a video codec, use the `-c <codec>` option. To modify codec parameters, use `-p <option_name>=<option_value>`.

You can also specify an audio codec, using `-C <codec>`. Alternatively, the long form `--audio-codec` can be used. 

### Supported video and audio codecs
You can use the following command to check all available video codecs
```
ffmpeg -hide_banner -encoders | grep -E '^ V' | grep -F '(codec' | cut -c 8- | sort
```

and the following for audio codecs

```
ffmpeg -hide_banner -encoders | grep -E '^ A' | grep -F '(codec' | cut -c 8- | sort
```

### Specifying the video muxer
Use ffmpeg to get details about specific encoder, filter or muxer.

To set a specific output format, use the `--muxer` option. For example, to output to a video4linux2 loopback you might use:
```
wf-recorder --muxer=v4l2 --codec=rawvideo --file=/dev/video2
```

### GPU-accelerated encoding with VAAPI
To use GPU encoding, use a VAAPI codec (for ex. `h264_vaapi`) and specify a GPU device to use with the `-d` option:
```
wf-recorder -f test-vaapi.mkv -c h264_vaapi -d /dev/dri/renderD128
```
Some drivers report support for rgb0 data for vaapi input but really only support yuv planar formats. In this case, use the `-x yuv420p` or `--pixel-format yuv420p` option in addition to the vaapi options to convert the data to yuv planar data before sending it to the GPU.

### More examples
More examples [can be found here](doc/examples.md)
