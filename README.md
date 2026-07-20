# waybar-volume

<p align="center"><img src="assets/icon.png" width="128" alt="waybar-volume icon"></p>

A [waybar](https://github.com/Alexays/Waybar) **CFFI plugin** for audio volume: a
speaker icon + level in the bar; scroll to adjust, right-click to mute, click for a
slider and an output-device picker. PipeWire via `wpctl`, event-driven off
`pactl subscribe` (no polling).

## Features

- **Bar pill:** level/muted speaker glyph + NN%.
- **Scroll** = ±5% (configurable), **right-click** = mute toggle.
- **Click → popover:** volume slider, mute button, output-device picker.
- Updates are event-driven off `pactl subscribe`.

## Build & install

Arch Linux: `yay -S waybar-volume` (AUR).

Requires `gtk3`, `glib2` (+dev headers), `wireplumber` (`wpctl`),
`pipewire-pulse` (provides the `pactl`-compatible PipeWire server; pulls in
`libpulse`, which owns the `pactl` binary itself) and a C compiler.

```sh
make
make install                 # → ~/.local/lib/waybar/libvolume.so
```

## waybar config

```jsonc
"modules-right": ["cffi/volume"],

"cffi/volume": {
    "module_path": "/home/YOU/.local/lib/waybar/libvolume.so",
    "scroll-step": 5
}
```

| key | default | meaning |
|-----|---------|---------|
| `module_path` | *(required)* | path to `libvolume.so` |
| `scroll-step` | 5 | volume % per scroll notch |

## style.css

Bar: `#volume` (state class `.muted`) with `.vo-icon` / `.vo-label`. Popover:
`.vo-pop`, `.vo-head`, `.vo-mute`, `.vo-scale`, `.vo-sink` (`.vo-active` on the
current output).

## License

MIT
