project(
	'winvnc',
	'c',
	version: '0.1.0',
	license: 'ISC',
	default_options: [
		'c_std=c11',
		'warning_level=2',
	]
)

cc = meson.get_compiler('c')
is_windows = host_machine.system() == 'windows'

# Dependencies
aml_dep = dependency('aml', fallback: ['aml', 'aml_dep'])
neatvnc_dep = dependency('neatvnc', fallback: ['neatvnc', 'neatvnc_dep'],
	required: true)
pixman_dep = dependency('pixman-1')

inc = include_directories('include')

sources = [
	'src/main.c',
]

c_args = []

if is_windows
	sources += [
		'src/dxgi-capture.c',
		'src/win-input.c',
	]
	c_args += ['-DBUILDING_WINVNC']
endif

executable(
	'winvnc',
	sources,
	include_directories: inc,
	dependencies: [aml_dep, neatvnc_dep, pixman_dep],
	c_args: c_args,
)
