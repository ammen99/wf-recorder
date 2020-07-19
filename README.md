# wf-recorder

wf-recorder is a utility program for screen recording of `wlroots`-based compositors (more specifically, those that support `wlr-screencopy-v1` and `xdg-output`). Its dependencies are `ffmpeg`, `wayland-client` and `wayland-protocols`.

# installation

## Arch Linux

Arch users can install wf-recorder from the Community repo.
```
pacman -S wf-recorder
```

## Artix Linux

Artix users can install wf-recorder from the official repos
```
pacman -S wf-recorder
```

## Void Linux

Void users can install wf-recorder from the official repos
```
xbps-install -S wf-recorder
```

## Fedora Linux

Fedora users can install from rpmfusion-free-updates. First [enable rpmfusion](https://rpmfusion.org/Configuration) and then
```
sudo dnf install wf-recorder
```

## From Source
### Install Dependencies

#### Ubuntu
```
sudo apt install libavutil-dev libavcodec-dev libavformat-dev libswscale-dev libpulse-dev
```

#### Fedora
```
$ sudo dnf install wayland-devel wayland-protocols-devel ffmpeg-devel
```

### Download & Build
```
git clone https://github.com/ammen99/wf-recorder.git && cd wf-recorder
meson build --prefix=/usr --buildtype=release
ninja -C build
```
Optionally configure with `-Ddefault_codec='codec'`. The default is libx264. Now you can just run `./build/wf-recorder` or install it with `sudo ninja -C build install`.

Optionally install `scdoc`, a tool by ddevault, for building the manpage.

# Usage
In its simplest form, run `wf-recorder` to start recording and use Ctrl+C to stop. This will create a file called `recording.mp4` in the current working directory using the default codec.

Use `-f <filename>` to specify the output file. In case of multiple outputs, you'll first be prompted to select the output you want to record. If you know the output name beforehand, you can use the `-o <output name>` option.

To select a specific part of the screen you can either use `-g <geometry>`, or use [slurp](https://github.com/emersion/slurp) for interactive selection of the screen area that will be recorded:

```
wf-recorder -g "$(slurp)"
```

You can record screen and sound simultaneously with

```
wf-recorder --audio --file=recording_with_audio.mp4
```

To specify a codec, use the `-c <codec>` option. To modify codec parameters, use `-p <option_name>=<option_value>`.

To set a specific output format, use the `--muxer` option. For example, to output to a video4linux2 loopback you might use:
```
wf-recorder --muxer=v4l2 --codec=rawvideo --file=/dev/video2
```

To use GPU encoding, use a VAAPI codec (for ex. `h264_vaapi`) and specify a GPU device to use with the `-d` option:
```
wf-recorder -f test-vaapi.mkv -c h264_vaapi -d /dev/dri/renderD128
```
Some drivers report support for rgb0 data for vaapi input but really only support yuv planar formats. In this case, use the `-t` or `--force-yuv` option in addition to the vaapi options to convert the data to yuv planar data before sending it to the GPU.

The `-e` option attempts to use OpenCL if wf-recorder was built with OpenCL support and `-t` or `--force-yuv` are specified, even without vaapi GPU encoding. Use `-e#` or `--opencl=#` to use a specific OpenCL device, where `#` is one of the devices listed.
