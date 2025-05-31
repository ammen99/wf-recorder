project(
	'wf-recorder',
    'c',
	'cpp',
	version: '0.5.0',
	license: 'MIT',
	meson_version: '>=0.54.0',
	default_options: [
		'cpp_std=c++17',
        'c_std=c11',
		'warning_level=2',
		'werror=false',
	],
)

conf_data = configuration_data()

conf_data.set('default_codec', get_option('default_codec'))
conf_data.set('default_pix_fmt', get_option('default_pixel_format'))
conf_data.set('default_audio_codec', get_option('default_audio_codec'))
conf_data.set('default_audio_sample_rate', get_option('default_audio_sample_rate'))
conf_data.set('default_container_format', get_option('default_container_format'))
conf_data.set('fallback_audio_sample_fmt', get_option('fallback_audio_sample_fmt'))

version = '"@0@"'.format(meson.project_version())
git = find_program('git', native: true, required: false)
if git.found()
	git_commit = run_command([git, 'rev-parse', '--short', 'HEAD'], check: false)
	git_branch = run_command([git, 'rev-parse', '--abbrev-ref', 'HEAD'], check: false)
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

project_sources = ['src/frame-writer.cpp', 'src/main.cpp', 'src/averr.c', 'src/debug.cpp']

wayland_client = dependency('wayland-client', version: '>=1.20')
wayland_protos = dependency('wayland-protocols', version: '>=1.14')

audio_backends = {
    'pulse': {
      'dependency': dependency('libpulse-simple', required: false),
      'sources': ['src/pulse.cpp'],
      'define': 'HAVE_PULSE'
    },
    'pipewire': {
      'dependency': dependency('libpipewire-0.3', version: '>=1.0.5', required: false),
      'sources': ['src/pipewire.cpp'],
      'define': 'HAVE_PIPEWIRE'
    }
}

default_audio_backend = get_option('default_audio_backend')
message('Using default audio backend: @0@'.format(default_audio_backend))
have_audio = false
audio_deps = []

foreach backend_name, backend_data : audio_backends
  if default_audio_backend == backend_name and not backend_data['dependency'].found()
    error('Default audio backend set to @0@, but @1@ dependency was not found!'.format(backend_name, backend_data['dependency'].name()))
  endif

  if default_audio_backend == backend_name and get_option(backend_name).disabled()
    error('Default audio backend set to @0@, but @1@ support is disabled!'.format(backend_name, backend_name))
  endif

  if get_option(backend_name).enabled() and not backend_data['dependency'].found()
    error('@0@ support is enabled, but @1@ dependency was not found!'.format(backend_name, backend_data['dependency'].name()))
  endif

  if backend_data['dependency'].found() and not get_option(backend_name).disabled()
    conf_data.set(backend_data['define'], true)
    project_sources += backend_data['sources']
    audio_deps += backend_data['dependency']
    have_audio = true
  else
    conf_data.set(backend_data['define'], false)
  endif
endforeach

if have_audio
    conf_data.set('HAVE_AUDIO', true)
    project_sources += 'src/audio.cpp'

    if default_audio_backend == 'auto'
      if conf_data.get('HAVE_PULSE')
        default_audio_backend = 'pulse'
      else
        foreach backend_name, backend_data : audio_backends
          if conf_data.get(backend_data['define'])
            default_audio_backend = backend_name
            break
          endif
        endforeach
      endif
    endif
endif

conf_data.set('default_audio_backend', default_audio_backend)

libavutil = dependency('libavutil')
libavcodec = dependency('libavcodec')
libavformat = dependency('libavformat')
libavdevice = dependency('libavdevice', required: false)
libavfilter = dependency('libavfilter')
swr = dependency('libswresample')
threads = dependency('threads')
gbm = dependency('gbm')
drm = dependency('libdrm')

conf_data.set('HAVE_LIBAVDEVICE', libavdevice.found())

configure_file(input: 'config.h.in',
               output: 'config.h',
               configuration: conf_data)

install_data('manpage/wf-recorder.1', install_dir :
	join_paths(get_option('prefix'), get_option('mandir'), 'man1'))

install_data('completions/wf-recorder.fish', install_dir :
	join_paths(get_option('prefix'), 'share/fish/fish/vendor_completions.d/'))

subdir('proto')

dependencies = [
    wayland_client, wayland_protos,
    libavutil, libavcodec, libavformat, libavdevice, libavfilter,
    wf_protos, threads, swr, gbm, drm
] + audio_deps

executable('wf-recorder', project_sources,
        dependencies: dependencies,
        install: true)

summary = [
	'',
	'----------------',
	'wf-recorder @0@'.format(meson.project_version()),
    '----------------',
    'Default audio backend: @0@'.format(default_audio_backend),
]

foreach backend_name, backend_data : audio_backends
  summary += ['  - @0@: @1@'.format(backend_name, conf_data.get(backend_data['define']))]
endforeach
message('\n'.join(summary))
