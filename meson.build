project(
	'wf-recorder',
    'c',
	'cpp',
	version: '0.2.1',
	license: 'MIT',
	meson_version: '>=0.47.0',
	default_options: [
		'cpp_std=c++11',
        'c_std=c11',
		'warning_level=2',
		'werror=false',
	],
)

conf_data = configuration_data()

conf_data.set('default_codec', get_option('default_codec'))

version = '"@0@"'.format(meson.project_version())
git = find_program('git', native: true, required: false)
if git.found()
	git_commit = run_command([git, 'rev-parse', '--short', 'HEAD'])
	git_branch = run_command([git, 'rev-parse', '--abbrev-ref', 'HEAD'])
	if git_commit.returncode() == 0 and git_branch.returncode() == 0
		version = '"@0@-@1@ (" __DATE__ ", branch \'@2@\')"'.format(
			meson.project_version(),
			git_commit.stdout().strip(),
			git_branch.stdout().strip(),
		)
	endif
endif

add_project_arguments('-DWFRECORDER_VERSION=@0@'.format(version), language: 'cpp')

include_directories(['.'])

add_project_arguments(['-Wno-deprecated-declarations'], language: 'cpp')

project_sources = ['src/frame-writer.cpp', 'src/main.cpp', 'src/averr.c']

wayland_client = dependency('wayland-client')
wayland_protos = dependency('wayland-protocols', version: '>=1.14')

pulse = dependency('libpulse-simple', required : get_option('pulse'))

if pulse.found()
    conf_data.set('HAVE_PULSE', true)
    project_sources += 'src/pulse.cpp'
endif

opencl = dependency('OpenCL', required : get_option('opencl'))

if opencl.found()
    conf_data.set('HAVE_OPENCL', true)
    project_sources += 'src/opencl.cpp'
endif

libavutil = dependency('libavutil')
libavcodec = dependency('libavcodec')
libavformat = dependency('libavformat')
libavdevice = dependency('libavdevice', required: false)
libavfilter = dependency('libavfilter')
swr = dependency('libswresample')
threads = dependency('threads')

conf_data.set('HAVE_LIBAVDEVICE', libavdevice.found())

configure_file(input: 'config.h.in',
               output: 'config.h',
               configuration: conf_data)

install_data('manpage/wf-recorder.1', install_dir :
	join_paths(get_option('prefix'), get_option('mandir'), 'man1'))

subdir('proto')

dependencies = [
    wayland_client, wayland_protos,
    libavutil, libavcodec, libavformat, libavdevice, libavfilter,
    wf_protos, threads, pulse, swr, opencl
]

executable('wf-recorder', project_sources,
        dependencies: dependencies,
        install: true)
