# wf-recorder

wf-recorder is a utility program for screen recording of `wlroots`-based compositors (more specifically, those that support `wlr-screencopy-v1` and `xdg-output`). Its dependencies are `ffmpeg`, `wayland-client` and `wayland-protocols`.

# installation

[comment]: <> (List ordered alphabetically)

## Alpine Linux

wf-recorder is available in the community repositories:
```
apk add wf-recorder
```

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

## Debian GNU/Linux

Debian users can install wf-recorder from official repos
```
apt install wf-recorder
```

## Fedora Linux

Fedora users can install wf-recorder from the official repos
```
sudo dnf install wf-recorder
```

## Gentoo Linux

Gentoo users can install wf-recorder from the official (`::gentoo`) repository.

## NixOS / Nix

Users of the Nix package manager can add the `wf-recorder` package to their system configurations, or use `nix-shell` / `nix shell` / `nix run`:

```
nix-shell -p wf-recorder
# OR
nix shell nixpkgs#wf-recorder
# OR
nix run nixpkgs#wf-recorder
```

## Void Linux

Void users can install wf-recorder from the official repos
```
xbps-install -S wf-recorder
```


## From Source
### Install Dependencies

#### Ubuntu
```
sudo apt install g++ meson libavutil-dev libavcodec-dev libavformat-dev libswscale-dev libpulse-dev
```

#### Fedora
```
$ sudo dnf install gcc-c++ meson wayland-devel wayland-protocols-devel ffmpeg-free-devel pulseaudio-libs-devel
```

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

To select a specific part of the screen you can either use `-g <geometry>`, or use [slurp](https://github.com/emersion/slurp) for interactive selection of the screen area that will be recorded:

```
wf-recorder -g "$(slurp)"
```

You can record screen and sound simultaneously with

```
wf-recorder --audio --file=recording_with_audio.mp4
```

To specify an audio device, use the `-a<device>` or `--audio=<device>` options.

To specify a video codec, use the `-c <codec>` option. To modify codec parameters, use `-p <option_name>=<option_value>`.

You can also specify an audio codec, using `-C <codec>`. Alternatively, the long form `--audio-codec` can be used. 

You can use the following command to check all available video codecs
```
ffmpeg -hide_banner -encoders | grep -E '^ V' | grep -F '(codec' | cut -c 8- | sort
```

and the following for audio codecs

```
ffmpeg -hide_banner -encoders | grep -E '^ A' | grep -F '(codec' | cut -c 8- | sort
```

Use ffmpeg to get details about specific encoder, filter or muxer.

To set a specific output format, use the `--muxer` option. For example, to output to a video4linux2 loopback you might use:
```
wf-recorder --muxer=v4l2 --codec=rawvideo --file=/dev/video2
```

To use GPU encoding, use a VAAPI codec (for ex. `h264_vaapi`) and specify a GPU device to use with the `-d` option:
```
wf-recorder -f test-vaapi.mkv -c h264_vaapi -d /dev/dri/renderD128
```
Some drivers report support for rgb0 data for vaapi input but really only support yuv planar formats. In this case, use the `-x yuv420p` or `--pixel-format yuv420p` option in addition to the vaapi options to convert the data to yuv planar data before sending it to the GPU.
