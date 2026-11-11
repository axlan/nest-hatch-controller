# hatch-bridge

Listens for the Nest picker's color on MQTT and sets a **Hatch Rest (gen 1)**
night light to that color over Bluetooth LE, using one persistent connection so
turning the ring feels live.

```bash
uv run hatch-bridge --broker 192.168.1.110 --hatch E3:3F:DB:C8:93:E2
```

- Hue and saturation set the color, and the picker's brightness (`v`) sets the Hatch brightness.
- Changing the color powers the Hatch on (`SI01`). **The Hatch's sound comes on with it**
  if a sound is selected in the Hatch app. Use `--no-power` to only recolor while it's already on.
- `v = 0` (brightness all the way down) powers the Hatch off.
- At startup the broker's retained color is ignored, so a restart never turns the light on.
  Pass `--apply-retained` to change that.
- Publishes `hatch/rest/state` (JSON: power, color, brightness, sound, volume) and
  `hatch/rest/status` (`online`/`offline`).
- Omit `--hatch` to use the first Hatch it finds (manufacturer ID 1076).

Every option also reads an env var (`MQTT_HOST`, `MQTT_PORT`, `MQTT_USER`, `MQTT_PASS`,
`COLOR_TOPIC`, `HATCH_ADDRESS`, `HATCH_TOPIC`). See `hatch-bridge.service` to run it under systemd.

Only one BLE central can be connected to the Hatch at a time. While the bridge is
running, the Hatch phone app can't reach it over Bluetooth.
