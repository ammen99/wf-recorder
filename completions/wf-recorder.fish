complete -c wf-recorder --no-files

complete -c wf-recorder -s a -l audio              -d 'Starts recording the screen with audio' --arguments '(pactl list sources | grep Name | cut -d " " -f 2)'
complete -c wf-recorder -s c -l codec              -d 'Specifies the codec of the video' --arguments '(ffmpeg -hide_banner -encoders | grep "^ V" | grep -F "(codec" | cut -c 9- | cut -d " " -f 1)' --exclusive
complete -c wf-recorder -s r -l framerate          -d 'Changes framerate to constant framerate with a given value' --exclusive
complete -c wf-recorder -s d -l device             -d 'Selects the device to use when encoding the video' --arguments '(find /dev/dri -type c)' --exclusive
complete -c wf-recorder      -l no-dmabuf          -d 'Disables GPU buffer usage, forcing CPU copy to avoid potential issues.'
complete -c wf-recorder -s D -l no-damage          -d 'Disables damage-based recording, capturing frames continuously for a constant refresh rate.'
complete -c wf-recorder -s f                       -d 'Sets the output file name and format based on the given extension.' --require-parameter --force-files
complete -c wf-recorder -s m -l muxer              -d 'Set the output format to a specific muxer' --arguments '(ffmpeg -hide_banner -muxers | grep "^  E" | cut -c 6- | cut -d " " -f 1 | tr "," "\n")' --exclusive
complete -c wf-recorder -s x -l pixel-format       -d 'Set the output pixel format' --arguments '(ffmpeg -hide_banner -pix_fmts | tail -n +9 | cut -d " " -f 2)' --exclusive
complete -c wf-recorder -s g -l geometry           -d 'Selects a specific part of the screen. The format is "x,y WxH".' --exclusive
complete -c wf-recorder -s h -l help               -d 'Prints help'
complete -c wf-recorder -s v -l version            -d 'Prints the version of wf-recorder'
complete -c wf-recorder -s l -l log                -d 'Generates a log on the current terminal'
complete -c wf-recorder -s o -l output             -d 'Specify the output where the video is to be recorded' --exclusive
complete -c wf-recorder -s p -l codec-param        -d 'Change the codec parameters. (ex. -p <option_name>=<option_value>)' --exclusive
complete -c wf-recorder -s F -l filter             -d 'Specify the ffmpeg filter string to use. (ex. -F scale_vaapi=format=nv12)' --exclusive
complete -c wf-recorder -s b -l bframes            -d 'This option is used to set the maximum number of b-frames to be used' --exclusive
complete -c wf-recorder -s B -l buffrate           -d 'This option is used to specify the buffers expected framerate' --exclusive
complete -c wf-recorder      -l audio-backend      -d 'Specifies the audio backend' --exclusive
complete -c wf-recorder -s C -l audio-codec        -d 'Specifies the codec of the audio' --exclusive
complete -c wf-recorder -s X -l sample-format      -d 'Set the output audio sample format' --arguments '(ffmpeg -hide_banner -sample_fmts | tail -n +2 | cut -d " " -f 1)' --exclusive
complete -c wf-recorder -s R -l sample-rate        -d 'Changes the audio sample rate in HZ. (default: 48000)' --exclusive
complete -c wf-recorder -s P -l audio-codec-param  -d 'Change the audio codec parameters. (ex.. -P <option_name>=<option_value>)' --exclusive
complete -c wf-recorder -s y -l overwrite          -d 'Force overwriting the output file without prompting'
