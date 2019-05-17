project(
	'wf-recorder',
    'c',
	'cpp',
	version: '0.1',
	license: 'MIT',
	meson_version: '>=0.43.0',
	default_options: [
		'cpp_std=c++11',
        'c_std=c11',
		'warning_level=2',
		'werror=false',
	],
)

conf_data = configuration_data()

conf_data.set('default_codec', get_option('default_codec'))

configure_file(input: 'config.h.in',
               output: 'config.h',
               configuration: conf_data)

include_directories(['.'])

add_project_arguments(['-Wno-deprecated-declarations'], language: 'cpp')

wayland_client = dependency('wayland-client')
wayland_protos = dependency('wayland-protocols')

libavutil = dependency('libavutil')
libavcodec = dependency('libavcodec')
libavformat = dependency('libavformat')
sws = dependency('libswscale')
swr = dependency('libswresample')
threads = dependency('threads')
pulse = dependency('libpulse-simple')

subdir('proto')
executable('wf-recorder', ['src/frame-writer.cpp', 'src/main.cpp', 'src/pulse.cpp', 'src/averr.c'],
        dependencies: [wayland_client, wayland_protos, libavutil, libavcodec, libavformat, wf_protos, sws, threads, pulse, swr],
        install: true)
