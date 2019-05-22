# wf-recorder

wf-recorder is a utility program for screen recording of `wlroots`-based compositors (more specifically, those that support `wlr-screencopy-v1` and `xdg-output`). Its dependences are `ffmpeg`, `wayland-client` and `wayland-protocols`.

# installation

## archlinux

Arch users can use [wf-recorder-git](https://aur.archlinux.org/packages/wf-recorder-git/) from the AUR.
```
yay -S wf-recorder-git 
```


## from source

```
git clone https://github.com/ammen99/wf-recorder && cd wf-recorder
meson build --prefix=/usr --buildtype=release
ninja -C build
```
Optionally configure with `-Ddefault_codec='codec'`. The default is libx264. Now you can just run `./build/wf-recorder` or install it with `sudo ninja -C build install`.

# usage
In it's simplest form, run `wf-recorder` to start recording and use Ctrl+C to stop. This will create a file called recording.mp4 in the current working directory using the default codec.

Use `-f <filename>` to specify the output file. In case of multiple outputs, you'll first be prompted to select the output you want to record. If you know the output name beforehand, you can use the `-o <output name>` option. 

To select a specific part of the screen you can either use the `-g <geometry>`, or use [slurp](https://github.com/emersion/slurp) for interactive selection of the area
```
wf-recorder -g "$(slurp)"
``` 
to select and limit the recording to a part of the screen.

To specify a codec, use the `-c <codec>` option. To modify codec parameters, use `-p <option_name>=<option_value>`

To use gpu encoding, use a VAAPI codec (for ex. `h264_vaapi`) and specify a GPU device to use with the `-d` option:
```
wf-recorder -f test-vaapi.mkv -c h264_vaapi -d /dev/dri/renderD128
```

## with [Sway](https://swaywm.org/)

In your Sway config file, add those key bindings to start and stop wf-recorder :

```
bindsym Ctrl+Print exec wf-recorder -f ~/recording_$(date +"%Y-%m-%d_%H:%M:%S.mp4")
bindsym Ctrl+Shift+Print exec wf-recorder -g "$$(slurp)" -f ~/recording_$(date +"%Y-%m-%d_%H:%M:%S.mp4")
bindsym Ctrl+Shift+BackSpace exec killall -s SIGINT wf-recorder
```

* `Ctrl+Print` will start recording the whole screen
* `Ctrl+Shift+Print` will let you select an area to record and start recording
* `Ctrl+Shift+BackSpace` will stop all recordings
