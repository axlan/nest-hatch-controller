# python/: PC-side tools

One uv project (package `nest_ui`) with three commands:

| Command | Module | What it does |
|---|---|---|
| `hatch-bridge` | [`hatch_bridge.py`](src/nest_ui/hatch_bridge.py) | The MQTT → Hatch Rest bridge in Python, for testing (the ESP32 firmware in `../hatch-ble-bridge` is the permanent one) |
| `hatch-ble-bridge-setup` | [`device_setup.py`](src/nest_ui/device_setup.py) | Find and configure the ESP32 bridge |
| `gif2anim` | [`gif2anim.py`](src/nest_ui/gif2anim.py) | Convert a GIF into the Nest's night-mode animation |

Run them from this directory with `uv run <command>`, or from the repo root with
`uv run --project python <command>`.

## hatch-bridge

Listens for the Nest picker's color and sound on MQTT and applies them to a
**Hatch Rest (gen 1)** over Bluetooth LE. It keeps one persistent connection open,
so turning the ring feels live.

```bash
uv run hatch-bridge --broker 192.168.1.110 --hatch E3:3F:DB:C8:93:E2
```

- It changes **only the color and the sound**. It never turns the Hatch on or off, and never
  changes brightness or volume: color is sent at the Hatch's current brightness. While the Hatch
  is off, the new color and sound are stored and used the next time it's switched on.
- Reads `nest/picker/color` (hue → full-intensity RGB) and `nest/picker/sound`
  (`{"id":3,"name":"noise"}`, a bare id, or a name such as `rain`).
- Sound changes wait until the selection has been stable for `--sound-delay` seconds (0.6),
  so scrolling past sounds doesn't play each one.
- On startup it applies the broker's retained color and sound. That's safe because it never powers the Hatch on.
- Publishes `hatch/rest/state` (JSON: power, color, brightness, sound, volume) and
  `hatch/rest/status` (`online`/`offline`).
- Omit `--hatch` to use the first Hatch it finds (manufacturer ID 1076).

Every option also reads an env var (`MQTT_HOST`, `MQTT_PORT`, `MQTT_USER`, `MQTT_PASS`,
`PICKER_TOPIC`, `HATCH_ADDRESS`, `HATCH_TOPIC`, `SOUND_DELAY`). See
[`hatch-bridge.service`](hatch-bridge.service) to run it under systemd.

Only one BLE central can be connected to the Hatch at a time. Don't run this and the ESP32
bridge together, and the Hatch phone app can't reach the Hatch over Bluetooth while either runs.

## hatch-ble-bridge-setup

Discovers and configures the ESP32-S3 bridge, following
[pcb-map](https://github.com/axlan/pcb-map)'s `device_setup.py`:

```bash
uv run hatch-ble-bridge-setup find-devices                        # mDNS → MQTT ping → setup-AP scan
uv run hatch-ble-bridge-setup status                              # WiFi / MQTT / Hatch status (JSON)
uv run hatch-ble-bridge-setup setup-mqtt -h 192.168.1.110         # broker (-p, -t, -u, -P)
uv run hatch-ble-bridge-setup setup-bridge --hatch-address auto   # or an address; also --picker-topic etc.
uv run hatch-ble-bridge-setup clear-wifi-creds                    # back to the setup AP
```

See [`../hatch-ble-bridge/README.md`](../hatch-ble-bridge/README.md) for the UDP protocol behind these.

## gif2anim

Converts a GIF (or a still GIF/PNG) into the night-mode animation the Nest picker plays.
`../install.sh --night-gif FILE` runs it for you, before anything is sent to the Nest. To check a
GIF first:

```bash
uv run gif2anim moon.gif /tmp/moon.anim --preview /tmp/moon.png
```

- Each frame is cropped to fill the 320x320 screen (scaled to cover, centered) and gets its own
  256-color palette. The output is uncompressed so the Nest can memory-map it.
- It rejects non-images, more than 80 frames, or output over 8 MB.
- `--preview` writes a contact sheet of up to 16 frames, masked to the round screen.

**GIF tips:** square and at least 320x320, with the subject inside the central circle, a dark
background, and a seamless loop of at most 80 frames.
