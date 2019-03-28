project(
	'wf-recorder',
    'c',
	'cpp',
	version: '0.0',
	license: 'MIT',
	meson_version: '>=0.43.0',
	default_options: [
		'cpp_std=c++11',
        'c_std=c11',
		'warning_level=2',
		'werror=false',
	],
)

add_project_arguments(['-Wno-deprecated-declarations'], language: 'cpp')

wayland_client = dependency('wayland-client')
wayland_protos = dependency('wayland-protocols')

libavutil = dependency('libavutil')
libavcodec = dependency('libavcodec')
libavformat = dependency('libavformat')
sws = dependency('libswscale')
swr = dependency('libswresample')
x264 = dependency('x264')
threads = dependency('threads')
pulse = dependency('libpulse-simple')

subdir('proto')
executable('wf-recorder', ['src/frame-writer.cpp', 'src/main.cpp', 'src/pulse.cpp'],
        dependencies: [wayland_client, wayland_protos, libavutil, libavcodec, libavformat, wf_protos, x264, sws, threads, pulse, swr],
        install: true)
