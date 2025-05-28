gst_vosk_sources = [
  'gstvosk.c',
  ]

vosk_libdir = meson.project_source_root() / 'vosk'
vosk_dep = declare_dependency(
	dependencies : cc.find_library('vosk', dirs : vosk_libdir),
	include_directories : include_directories('../vosk/'),
)

rnnoise_dep = dependency('rnnoise', required: false)
libm_dep = cc.find_library('m', required: rnnoise_dep.found())

deps = [gst_dep, gio_dep, vosk_dep]
if rnnoise_dep.found()
  message('RNNoise Support Enabled')
  add_project_arguments('-DHAVE_RNNOISE=1', language: 'c')
  deps += [rnnoise_dep, libm_dep]
else
  message('RNNoise Support Disabled')
endif

gstvosk = library('gstvosk',
  gst_vosk_sources,
  c_args: plugin_c_args,
  dependencies : deps,
  install : true,
  install_dir : plugin_install_dir,
)
