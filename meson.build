project('swizzle', 'c',
  version: '0.1.0',
  license: 'AGPL-3.0-or-later',
  meson_version: '>=0.60.0',
  default_options: ['c_std=c11', 'warning_level=2', 'werror=false', 'buildtype=release', 'b_lto=true'])

cc = meson.get_compiler('c')
add_project_arguments(
  '-DWLR_USE_UNSTABLE',
  '-D_POSIX_C_SOURCE=200809L',
  '-DSWIZZLE_VERSION="@0@"'.format(meson.project_version()),
  '-DSWIZZLE_DATADIR="@0@"'.format(get_option('prefix') / get_option('datadir') / 'swizzle'),
  '-Wno-unused-parameter',
  '-Wno-missing-field-initializers',
  language: 'c')

wlroots = dependency('wlroots-0.20', version: ['>=0.20.0', '<0.21.0'])
wayland_server = dependency('wayland-server')
wayland_protos = dependency('wayland-protocols', version: '>=1.40')
xkbcommon = dependency('xkbcommon')
libinput = dependency('libinput')
libdrm = dependency('libdrm')
pixman = dependency('pixman-1')
math = cc.find_library('m', required: false)

deps = [wlroots, wayland_server, wayland_protos, xkbcommon, libinput, libdrm, pixman, math]

scenefx = dependency('scenefx-0.5', required: get_option('scenefx'))
if scenefx.found()
  add_project_arguments('-DWITH_SCENEFX', language: 'c')
  deps = [scenefx] + deps
endif

xwayland = get_option('xwayland')
wlr_has_xwayland = cc.get_define('WLR_HAS_XWAYLAND', prefix: '#include <wlr/config.h>', dependencies: wlroots) == '1'
if xwayland.enabled() and not wlr_has_xwayland
  error('xwayland requested but wlroots was built without it')
endif
if not xwayland.disabled() and wlr_has_xwayland
  add_project_arguments('-DXWAYLAND', language: 'c')
  deps += dependency('xcb')
  deps += dependency('xcb-icccm')
endif

scanner = find_program(dependency('wayland-scanner', native: true).get_variable('wayland_scanner'))
wl_protocol_dir = wayland_protos.get_variable('pkgdatadir')
proto_xml = [
  'protocols/wlr-layer-shell-unstable-v1.xml',
  'protocols/wlr-output-power-management-unstable-v1.xml',
  wl_protocol_dir / 'staging/ext-workspace/ext-workspace-v1.xml',
  wl_protocol_dir / 'stable/xdg-shell/xdg-shell.xml',
]

src = files(
  'src/main.c',
  'src/output.c',
  'src/input.c',
  'src/client.c',
  'src/shell.c',
  'src/config.c',
  'src/ipc.c',
)

optional = {
  'scratchpad': [],
  'sticky': [],
  'targets': [],
  'tablet': ['src/tablet.c', 'stable/tablet/tablet-v2.xml'],
  'lid': ['src/lid.c'],
  'window_share': ['src/share.c'],
  'portal': ['src/portal.c'],
  'presentation': ['src/presentation.c', 'staging/content-type/content-type-v1.xml'],
  'vr': ['src/lease.c'],
  'effects': ['src/effects.c'],
  'overview': ['src/overview.c'],
}
foreach name, parts : optional
  if get_option(name)
    add_project_arguments('-DWITH_' + name.to_upper(), language: 'c')
    foreach part : parts
      if part.endswith('.c')
        src += files(part)
      else
        proto_xml += wl_protocol_dir / part
      endif
    endforeach
  endif
endforeach
foreach layout : get_option('layouts')
  add_project_arguments('-DWITH_' + layout.to_upper(), language: 'c')
endforeach

proto_src = []
foreach xml : proto_xml
  name = xml.split('/')[-1].replace('.xml', '')
  proto_src += custom_target(name + '-protocol.h',
    input: xml, output: name + '-protocol.h',
    command: [scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
endforeach

executable('swizzle', src + proto_src, dependencies: deps, install: true)
executable('swizzlectl', 'swizzlectl/swizzlectl.c', install: true)

install_data('swizzle.conf', install_dir: get_option('datadir') / 'swizzle')
install_data('swizzle.desktop', install_dir: get_option('datadir') / 'wayland-sessions')
if get_option('portal')
  install_data('swizzle-portals.conf', install_dir: get_option('datadir') / 'xdg-desktop-portal')
endif
