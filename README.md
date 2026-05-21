# squigwire
Moved to https://codeberg.org/mazylol/squigwire

System-wide PipeWire parametric EQ with:
- multiband filter loading
- named presets from config
- live preset switching/reload from CLI

## Why?
I built this due to my frustrations with EasyEffects. While it is an amazing piece of software, I only need PEQ. Too much overhead for so little. My use case involves eqing headphones/iems via squiglink. This handles that perfectly for me. If all you want is the ability to peq your equipment on pipewire with the ability to switch presets on the fly, this is for you.

## Build

```bash
meson setup buildDir
ninja -C buildDir
```

## Install

```bash
sudo meson install -C buildDir
```

## Configure

Create config directory and config file:

```bash
mkdir -p ~/.config/squigwire
cp /usr/local/share/doc/squigwire/squigwire.conf.example ~/.config/squigwire/squigwire.conf
```

Edit `~/.config/squigwire/squigwire.conf`:

```ini
default_preset=ath-m40x
preset.ath-m40x=~/Downloads/Audio Technica ATH-M40x Filters (Harman 2018 aeoe).txt
# preset.other=~/path/to/other-filter.txt
```

AutoEQ filter files should look like:

```text
Preamp: -7.7 dB
Filter 1: ON PK Fc 21 Hz Gain 7.7 dB Q 1.300
...
```

## Run as user service (system-wide route)

```bash
systemctl --user daemon-reload
systemctl --user enable --now squigwire.service
```

## Dev mode (run from build dir)

If you want no reinstall loop during development:

```bash
systemctl --user edit squigwire.service
```

Use:

```ini
[Service]
ExecStart=
ExecStartPost=
ExecStopPost=
ExecStart=/home/mazylol/squigwire/buildDir/squigwire --daemon
ExecStartPost=/home/mazylol/squigwire/scripts/squigwire-route.sh up
ExecStopPost=/home/mazylol/squigwire/scripts/squigwire-route.sh down
```

Then:

```bash
systemctl --user daemon-reload
systemctl --user restart squigwire.service
```

## CLI usage

Use the same `squigwire` binary for daemon + control:

```bash
squigwire --daemon
squigwire preset list
squigwire preset current
squigwire preset set <name>
squigwire preset reload
```

Optional config override:

```bash
squigwire preset list --config /path/to/squigwire.conf
```
