project('squigwire', 'c', default_options: ['c_std=gnu11'])

pipewire_dep = dependency('libpipewire-0.3', required: true)
spa_dep = dependency('libspa-0.2', required: true)

cc = meson.get_compiler('c')
math_dep = cc.find_library('m', required: true)

src = ['main.c']

executable('squigwire', sources: src, dependencies: [pipewire_dep, spa_dep, math_dep], install: true)

install_data(
  'scripts/squigwire-route.sh',
  install_dir: get_option('bindir'),
  rename: 'squigwire-route',
  install_mode: ['rwxr-xr-x'],
)

service_conf = configuration_data()
service_conf.set('bindir', join_paths(get_option('prefix'), get_option('bindir')))

configure_file(
  input: 'systemd/squigwire.service.in',
  output: 'squigwire.service',
  configuration: service_conf,
  install: true,
  install_dir: join_paths(get_option('prefix'), 'lib/systemd/user'),
)

install_data(
  'systemd/squigwire.env.example',
  'systemd/squigwire.conf.example',
  install_dir: join_paths(get_option('datadir'), 'doc', meson.project_name()),
)
