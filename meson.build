project('gnome-c-desktop', 'c',
  version : '0.1.0',
  license : 'MIT',
  meson_version : '>=0.60.0',
  default_options : [
    'c_std=c11',
    'warning_level=2',
    'werror=false',
    'buildtype=debugoptimized',
  ])

add_project_arguments(
  '-DG_LOG_USE_STRUCTURED',
  '-D_GNU_SOURCE',
  language : 'c')

cc = meson.get_compiler('c')

# ---------------------------------------------------------------- dependencies

glib_dep    = dependency('glib-2.0',    version : '>=2.66')
gobject_dep = dependency('gobject-2.0')
gio_dep     = dependency('gio-2.0')
cairo_dep   = dependency('cairo')

x11_opt     = get_option('x11')
wayland_opt = get_option('wayland')

xcb_dep       = dependency('xcb',       required : x11_opt)
cairo_xcb_dep = dependency('cairo-xcb', required : x11_opt)
build_x11     = xcb_dep.found() and cairo_xcb_dep.found()

wl_client_dep = dependency('wayland-client', required : wayland_opt)
wl_scanner    = find_program('wayland-scanner', required : wayland_opt)
build_wayland = wl_client_dep.found() and wl_scanner.found()

if not build_x11 and not build_wayland
  error('Neither the X11 nor the Wayland backend can be built. ' +
        'Install libxcb + cairo-xcb and/or wayland-client + wayland-scanner.')
endif

conf = configuration_data()
conf.set10('GCD_HAVE_X11',     build_x11)
conf.set10('GCD_HAVE_WAYLAND', build_wayland)
conf.set_quoted('GCD_VERSION',  meson.project_version())
conf.set_quoted('GCD_LIBEXECDIR',
                join_paths(get_option('prefix'), get_option('libexecdir')))
conf.set_quoted('GCD_PREFIX', get_option('prefix'))
configure_file(output : 'gcd-config.h', configuration : conf)
config_inc = include_directories('.')

inc = include_directories('include')

# ------------------------------------------------------- wayland protocol gen

wl_proto_srcs = []
if build_wayland
  # layer-shell references xdg_popup — generate xdg-shell too so the
  # generated objects link
  wl_protocols_dep = dependency('wayland-protocols')
  wl_pkgdatadir = wl_protocols_dep.get_variable('pkgdatadir')
  protocols = [
    join_paths(wl_pkgdatadir, 'stable', 'xdg-shell', 'xdg-shell.xml'),
    'protocols/wlr-layer-shell-unstable-v1.xml',
    'protocols/wlr-foreign-toplevel-management-unstable-v1.xml',
  ]
  foreach xml : protocols
    base = xml.split('/')[-1].split('.')[0]
    wl_proto_srcs += custom_target(
      base + '-client-header',
      input   : xml,
      output  : base + '-client-protocol.h',
      command : [wl_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])
    wl_proto_srcs += custom_target(
      base + '-private-code',
      input   : xml,
      output  : base + '-protocol.c',
      command : [wl_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
  endforeach
endif

# ------------------------------------------------------------------ libraries

core_srcs = files(
  'src/core/gcd-json.c',
  'src/core/gcd-model.c',
  'src/core/gcd-window.c',
)

libgcdcore = static_library('gcdcore', core_srcs,
  include_directories : [inc, config_inc],
  dependencies        : [glib_dep, gobject_dep, gio_dep],
  install             : false)

gcdcore_dep = declare_dependency(
  link_with           : libgcdcore,
  include_directories : [inc, config_inc],
  dependencies        : [glib_dep, gobject_dep, gio_dep])

backend_srcs = []
backend_deps = [gcdcore_dep, cairo_dep]

if build_x11
  backend_srcs += files('src/backend/gcd-backend-x11.c')
  backend_deps += [xcb_dep, cairo_xcb_dep]
endif
if build_wayland
  backend_srcs += files('src/backend/gcd-backend-wayland.c') + wl_proto_srcs
  backend_deps += [wl_client_dep]
endif
backend_srcs += files('src/backend/gcd-backend.c')

libgcdbackend = static_library('gcdbackend', backend_srcs,
  include_directories : [inc, config_inc],
  dependencies        : backend_deps,
  install             : false)

gcdbackend_dep = declare_dependency(
  link_with           : libgcdbackend,
  include_directories : [inc, config_inc],
  dependencies        : backend_deps)

sync_srcs = files(
  'src/sync/gcd-sync-node.c',
  'src/sync/gcd-sync-proto.c',
  'src/kwin/gcd-kwin.c',
)

libgcdsync = static_library('gcdsync', sync_srcs,
  include_directories : [inc, config_inc],
  dependencies        : [gcdcore_dep, gio_dep],
  install             : false)

gcdsync_dep = declare_dependency(
  link_with           : libgcdsync,
  include_directories : [inc, config_inc],
  dependencies        : [gcdcore_dep, gio_dep])

# ------------------------------------------------------------------ binaries

executable('gcd-shell',
  files('src/shell/gcd-panel.c', 'src/shell/gcd-shell.c'),
  dependencies : [gcdcore_dep, gcdbackend_dep, gcdsync_dep, cairo_dep,
                  gio_dep],
  install : true)

executable('gcd-syncd',
  files('src/sync/gcd-syncd.c'),
  dependencies : [gcdcore_dep, gcdbackend_dep, gcdsync_dep, gio_dep],
  install : true)

executable('gcd-ctl',
  files('src/sync/gcd-ctl.c'),
  dependencies : [gcdcore_dep, gcdsync_dep],
  install : true)

# KWin bridge script, installed where gcd-kwin.c looks for it.
install_data('src/kwin/gcd-kwin-bridge.js',
  install_dir : join_paths(get_option('prefix'), 'share', 'gnome-c-desktop'))

# -------------------------------------------------------------------- tests

if get_option('tests')
  test_env = environment()
  test_env.set('G_MESSAGES_DEBUG', 'all')

  foreach t : [['test-json',        'tests/test-json.c'],
               ['test-window-model','tests/test-window-model.c'],
               ['test-sync-proto',  'tests/test-sync-proto.c']]
    e = executable(t[0], t[1],
      dependencies : [gcdcore_dep, gcdsync_dep],
      install : false)
    test(t[0], e, env : test_env, timeout : 30)
  endforeach

  if build_x11
    xvfb = find_program('xvfb-run', required : false)
    fake_wm = executable('fake-wm', 'tests/fake-wm.c',
      dependencies : [xcb_dep], install : false)
    if xvfb.found()
      test('x11-ewmh-smoke',
        find_program('tests/x11-smoke.sh'),
        args : [meson.current_build_dir()],
        env  : test_env,
        timeout : 60)
    endif
  endif
endif

summary = [
  '',
  '  gnome-c-desktop @0@'.format(meson.project_version()),
  '    X11 backend     : @0@'.format(build_x11),
  '    Wayland backend : @0@'.format(build_wayland),
  '    Tests           : @0@'.format(get_option('tests')),
  '',
]
message('\n'.join(summary))
