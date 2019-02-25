# wf-recorder

wf-recorder is a utility program for screen recording of `wlroots`-based compositors (more specifically, those that support `wlr-screencopy-v1` and `xdg-output`). Its dependences are `ffmpeg`, `wayland-client` and `wayland-protocols`.

# installation

## archlinux

Arch users can use [wf-recorder-git](https://aur.archlinux.org/packages/wf-recorder-git/) from the AUR.
```
yay -S wf-recorder-git #replace yay by your favorite AUR helper
```


## from source

```
git clone https://github.com/ammen99/wf-recorder && cd wf-recorder
meson build --prefix=/usr --buildtype=release
ninja -C build
```
Now you can just run `./build/wf-recorder` or install it with `sudo ninja -C build install`.

# usage
Use `-f <filename>` to specify the output file. In case of multiple outputs, you'll first be prompted to select the output you want to record. If you know the output name beforehand, you can use the `-o <output name>` option. 

You can use `wf-recorder -g (slurp)` to select and limit the recording to a part of the screen.
