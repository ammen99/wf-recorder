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

scdoc = dependency('scdoc', version: '>=1.9.2', native: true, required: get_option('man-pages'))

if scdoc.found()
    scdoc_bin=find_program(scdoc.get_pkgconfig_variable('scdoc'), native: true)

    sh = find_program('sh', native: true)

    mandir=get_option('mandir')

    filename ='manpage/wf-recorder.1.scd'

    project_name='wf-recorder'
    section='1'
    output= '@0@.@1@'.format(project_name, section)

    custom_target(
  	project_name,
	input: filename,
	output:output,
	command: [
	      sh, '-c', '@0@ < @INPUT@ > @1@'.format(scdoc_bin.path(), output)
	  ],
	install: true,
	install_dir: '@0@/man@1@'.format(mandir, section)
	)
endif

subdir('proto')
executable('wf-recorder', ['src/frame-writer.cpp', 'src/main.cpp', 'src/pulse.cpp', 'src/averr.c'],
        dependencies: [wayland_client, wayland_protos, libavutil, libavcodec, libavformat, wf_protos, sws, threads, pulse, swr],
        install: true)
