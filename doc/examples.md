# Video and Audio Recording Examples
Below is a more extensive list of recording examples.

### Table of Contents
* [Find an encoder's parameters](#find-an-encoders-parameters)
* [Setting video resolution and scale](#setting-video-resolution-and-scale)
* [Fast x264 recording](#fast-x264-recording)
* [Lossless video and RGB recording (H.264)](#lossless-video-and-rgb-recording-h264)
* [Lightweight video for editing](#lightweight-video-for-editing)
* [Lossless audio](#lossless-audio)

## Find an encoder's parameters
FFmpeg lets you see what parameters, pixel formats and sample formats a particular encoder supports.
The base for the command is `ffmpeg -h encoder=<encoder-name>`, for example:
```
ffmpeg -h encoder=libx264
ffmpeg -h encoder=flac
ffmpeg -h encoder=libvpx-vp9
ffmpeg -h encoder=png
```


## Setting video resolution and scale
You can scale your video's final resolution with an FFmpeg filter:

### Specified resolution
You can specify the width and height of your video, in this example to 1920x1080:
```
wf-recorder -F scale=1920:1080
```

### Specify width or height, keep aspect ratio
You can set the width or height while keeping the aspect ratio, the other dimension is calculated:
```sh
wf-recorder -F scale=1920:-1 # Specifies width as 1920
wf-recorder -F scale=-1:1080 # Specifies height as 1080
```

### Multiply or divide resolution
```sh
wf-recorder -F scale=iw/2:ih/2 # Divides the resolution by 2
wf-recorder -F scale=iw*3:ih*3 # Multiplies the resolution by 3
```

### Scaling filters
The scale filter uses the `sws_flags` parameter to determine the scaling algorithm. By default, bicubic is used.
You can find out which filters are supported with `ffmpeg -h filter=scale`.

Examples:
```sh
wf-recorder -F scale=iw*2:ih*w -p sws_flags=neighbor # Scales the video by 2 with nearest neighbor scaling
wf-recorder -F scale=1920:1080 -p sws_flags=bilinear # Scales the video to 1920x1080 with bilinear scaling
wf-recorder -F scale=1920:1080 -p sws_flags=lanczos # Scales the video to 1920x1080 with lanczos scaling
```


## Fast x264 recording
By default, x264 uses the `medium` preset which is very slow for the CPU, not so appropriate for realtime recording.
As you raise the preset from `veryfast` to `placebo`, the difference in compression efficiency and file size is very slight, sometimes negligible.

The biggest difference comes in going from `ultrafast` to `superfast`, and `superfast` to `veryfast` has some improvements too.

Using `superfast` or `veryfast` is ideal for realtime recording, though you can also resort to `ultrafast` if your CPU cannot keep up. You can set the preset with:
```
wf-recorder -c libx264 -p preset=superfast
```


## Lossless video and RGB recording (H.264)
### Lossless
The `x264` encoder can record in lossless compression. The `CRF` value adjusts the video's control rate factor, in other words the quality of your video.
CRF keeps the quality always the same throughout the video, while the bitrate (and file size) varies depending on how much data the video needs to achieve this level.

A CRF value of 0 results in lossless compression:
```
wf-recorder -c libx264 -p crf=0
```

### RGB
By default, x264 uses the YUV pixel format, which can at worst result in less color information (if you use subsampling instead of YUV 4:4:4) or at best result in some color destruction (RGB to YUV conversion).

You can record in x264 using the RGB pixel format by using the `libx264rgb` encoder:
```
wf-recorder -c libx264rgb
```

The pixel format does not need to be specified. Recording in RGB also fixes other inaccurate video colors you might find.

### True lossless
Combining lossless and RGB encoding results in a video 100% identical to what you see on your screen:
```
wf-recorder -c libx264rgb -p crf=0
```

Expect your video's file size to be massive.


## Lightweight video for editing
Video editing can be very heavy, especially with high-resolution and high-framerate videos.
Below are some ways to make your video faster to process in your editor. Note that all of these methods result in much bigger file sizes.

### Disable inter-frame compression
Inter-frame compression is a technique where frames share identical pixels with each other for sparing on bitrate. This makes the video slower to read.

You can disable inter-frame compression with the `g` parameter:
```sh
wf-recorder -c libx264 -p g=20 # Sets the keyframe interval to every 20 frames (lower than usual), the lower it is the faster it is to decode the video
wf-recorder -c libx264 -p g=0 # Disables inter-frame compression entirely
```

### Use an intermediate format
Intermediate video formats are video encoding formats that have no inter-frame compression but also have very lightweight compression, resulting in very fast video playback at the cost of massive file sizes.
These video formats also work with DaVinci Resolve on Linux.

Here's some examples of intermediate formats:
```sh
wf-recorder -c dnxhd -p profile=dnxhr_hq # Very popular in professional video
wf-recorder -c dnxhd -p profile=dnxhr_444 # Same as above, but YUV 4:4:4 pixels and much higher bitrate and encoding overhead

wf-recorder -c cfhd -p quality=high # GoPro Cineform, also very good. The quality parameter defines the bitrate
wf-recorder -c cfhd -p quality=film3+ # The highest bitrate, use with caution
wf-recorder -c cfhd -p quality=high -x gbrp12le # 12bit/channel RGB, very heavy
```


## Lossless audio
FLAC is a lossless audio compression format, you can use it with:
```
wf-recorder -a -C flac
```

By default, the audio is recorded at 16bit. You can record as s32 (24bit) as well:
```
wf-recorder -a -C flac -X s32
```
