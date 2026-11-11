# hatch-bridge

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
`PICKER_TOPIC`, `HATCH_ADDRESS`, `HATCH_TOPIC`, `SOUND_DELAY`). See `hatch-bridge.service` to
run it under systemd.

Only one BLE central can be connected to the Hatch at a time. While the bridge is
running, the Hatch phone app can't reach it over Bluetooth.

## ESP32 bridge setup tool

This project also provides `hatch-ble-bridge-setup`, which discovers and configures the
ESP32-S3 version of this bridge (`../hatch-ble-bridge`). See that README for details.
