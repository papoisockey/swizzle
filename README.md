# swizzle

Small dynamic tiling Wayland compositor on wlroots 0.20. dwl style tags, thirteen layouts, an overview and a scratchpad, one config file, no bar.

> **Warning:** the code is written entirely by an AI. Every decision about what it does and how it works is made by a person.

## Build

```sh
meson setup build
ninja -C build
sudo ninja -C build install
```

Needs wlroots 0.20, wayland, wayland-protocols, libinput, xkbcommon, libdrm, pixman, and xcb-icccm for XWayland.
With scenefx 0.5 installed it adds rounded corners and blur and renders with gles only. `-Dscenefx=disabled` leaves it out.

Optional parts, on by default: `xwayland` `layouts` `scratchpad` `sticky` `targets` `lid` `window_share` `portal` `presentation` `effects` `overview`. Off by default: `tablet` `vr`.

## Use

Run `swizzle` from a tty or pick it in a display manager.

```sh
swizzle --init       # copy the default config to ~/.config/swizzle/swizzle.conf
swizzle --validate   # check it, super+shift+r reloads it
```

## Config

Ini: a `[section]`, then `key=value`. Booleans are `1`/`0`, colors are `#rrggbb` or `#rrggbbaa`.

```ini
[general]
gaps=8
border_focus=#88c0d0

[output DP-1]
mode=2560x1440@144
layout=scroll

[rule]
app_id=mpv
floating=1

[bind]
super+Return=spawn foot
locked+XF86AudioMute=spawn wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle
```

| Section | Holds |
|---|---|
| `[general]` | look, layouts, effects, focus |
| `[input]` `[input touchpad]` `[input pointer]` | keyboard, every pointer, one device class |
| `[output *]` `[output NAME]` | mode, scale, position, transform, layout, `tag_layouts`, vrr, tearing |
| `[rule]` | `app_id`, `title`, then `floating`, `fullscreen`, `sticky`, `scratchpad`, `focus`, `tags`, `output` |
| `[layer]` | `namespace`, then `animations`, `blur`, `shadow`, `animation_open`, `animation_close` |
| `[autostart]` `[env]` | `exec=CMD`, `NAME=VALUE` |
| `[bind]` | `MODS+KEY=COMMAND`, KEY is an xkb name, `wheel_up`, `button_middle`, `swipe:3:left`, `lid:close`; a `locked+` prefix keeps it working behind the lock screen, `locked+lid:close` included |

Layouts: tile, right_tile, vertical_tile, center_tile, deck, vertical_deck, dwindle, monocle, grid, vertical_grid, scroll, vertical_scroll, float.

## swizzlectl

```sh
swizzlectl view 3              # any binding command
swizzlectl on mpv fullscreen   # on every window with that app id
swizzlectl status              # monitors and windows
swizzlectl subscribe           # stream of tags, focus, title, layout events
```

## Notes

- The default config uses foot, wofi, wpctl, playerctl and brightnessctl.
- Screen sharing needs xdg-desktop-portal-wlr and xdg-desktop-portal-gtk.
- On virtual machine gpus (VirtualBox, VMware, QXL, bochs) it renders with pixman and draws the cursor in software; `renderer = gles` in `[general]` and `hardware_cursor = 1` in `[output *]` turn that back. A config copied before this needs `hardware_cursor = auto`. For VMs build with `-Dscenefx=disabled`, scenefx only renders with gles.
- Vulkan headers are not needed to build; whether libvulkan is needed at run time depends on how wlroots was built (`-Drenderers=gles2` leaves it out).
- The log is in `~/.local/state/swizzle/swizzle.log`, attach it to bug reports.
- Ideas from dwl, niri and KWin. AGPL-3.0-or-later, the files in `protocols/` keep their own license.
