wf-recorder(1)

# NAME

wf-recorder - A simple screen recording program for wlroots-based compositors

# SYNOPSIS

*wf-recorder* [options...] -f [file]

In its simplest form, run `wf-recorder` to start recording and use Ctrl+C to stop.
This will create a file called `recording.mp4` in the current working directory using
the default codec.

# OPTIONS
*-a, --audio [DEVICE]*
	Starts recording the screen with audio.

	*[DEVICE]* argument is optional.
	In case you want to specify the PulseAudio device which will capture the audio,
	you can run this command with the name of that device.
	You can find your device by running *pactl list sinks | grep Name*

*-c, --codec*
	Specifies the codec of the video. Supports GIF output as well.

	To modify codec parameters, use *-p <option_name>=<option_value>*

*-d, --device*
	Selects the device to use when encoding the video.

	Some drivers report support for rgb0 data for vaapi input but really only support yuv.

	Use the -t or --to-yuv option in addition to the vaapi options to convert the
	data in software, before sending it to the GPU.

*-f <filename>.ext*
	By using the -f option, the output file will have the name filename.ext and the file format
	will be determined by the provided extension. If the extension is not recognized by your
	FFmpeg muxers, the command will fail.

	You can check the muxers that your FFmpeg installation supports by running *ffmpeg -muxers*

*-m, --muxer*
	Set the output format to a specific muxer instead of detecting it from the filename.

*-x, --pixel-format*
	Set the output pixel format. List available formats using *ffmpeg -pix_fmts*

*-g, --geometry*
	Selects a specific part of the screen.

*-h, --help*
	Prints this help screen.

*-l, --log*
	Generates a log on the current terminal. For debug purposes.

*-o, --output*
	Specify the output where the video is to be recorded.

*-p, --codec-param*
	Change the codec parameters.

	*-p <option_name>=<option_value>*

*-e, --opencl [DEVICE]*
	Attempts to use OpenCL if wf-recorder was built with OpenCL support and `-t`
	or `--force-yuv` are specified, even without vaapi GPU encoding. Use `-e#` or `--opencl=#`
	to use a specific OpenCL device, where `#` is one of the devices listed.

*-t, --force-yuv*
	Use the -t or --force-yuv option in addition to the vaapi options to convert the data in software,
	before sending it to the GPU.

*-b [max_b_frames], --bframes [max_b_frames]*
	Sets the maximum number of B-Frames to use.

# DESCRIPTION
*wf-recorder* is a tool built to record your screen on Wayland compositors.
It makes use of wlr-screencopy for capturing video and FFmpeg for encoding it.

# EXAMPLES

To select a specific part of the screen you can either use `-g <geometry>`, or
use https://github.com/emersion/slurp for interactive selection of the
screen area that will be recorded:

```
wf-recorder -g "$(slurp)"
```

You can record screen and sound simultaneously with

```
wf-recorder --audio --file=recording_with_audio.mp4
```

To specify a codec, use the `-c <codec>` option. To modify codec parameters,
use `-p <option_name>=<option_value>`.

To set a specific output format, use the `--muxer` option. For example, to
output to a video4linux2 loopback you might use:

```
wf-recorder --muxer=v4l2 --codec=rawvideo --file=/dev/video2
```

To use GPU encoding, use a VAAPI codec (for ex. `h264_vaapi`) and specify a GPU
device to use with the `-d` option:

```
wf-recorder -f test-vaapi.mkv -c h264_vaapi -d /dev/dri/renderD128
```

Some drivers report support for rgb0 data for vaapi input but really only
support yuv planar formats. In this case, use the `-t` or `--force-yuv` option
in addition to the vaapi options to convert the data to yuv planar data before
sending it to the GPU.



# SEE ALSO

*ffmpeg*(1) *pactl*(1)
