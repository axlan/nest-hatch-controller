# hatch-ble-bridge (ESP32-S3)

Firmware that does what the Python `hatch-bridge` does, on an ESP32-S3. It
subscribes to the Nest picker's color and sound on MQTT and applies them to a
**Hatch Rest (gen 1)** over BLE.

- Only **color and sound** are changed. It never powers the Hatch on/off and never
  touches brightness or volume: color is sent with the Hatch's current brightness. Commands
  sent while the Hatch is off are stored without turning it on.
- Sound changes are debounced (600 ms) so scrolling past sounds doesn't play each one.
- After a color change it re-reads the Hatch's state, because the Hatch doesn't notify on
  color changes. So `hatch/rest/state` stays accurate.
- Bluetooth runs in its own FreeRTOS task with one persistent connection. A gen 1 Hatch can
  take ~20 s to accept a connection, and that must not stall MQTT.
- Only one BLE central can be connected to a Hatch at a time. Stop the Python bridge (and the
  Hatch phone app) while this runs.

## MQTT

| Topic | Dir | Payload |
|---|---|---|
| `nest/picker/color` | in | `{"h":210,...}` or `{"r":..,"g":..,"b":..}` |
| `nest/picker/sound` | in | `{"id":3,"name":"noise"}`, a bare id, or a name |
| `hatch/rest/state` | out | retained `{"power","hex","r","g","b","brightness","sound","volume"}` |
| `hatch/rest/status` | out | retained `online` / `offline` (Last Will) |
| `hatch-ble-bridge/ping` → `hatch-ble-bridge/pong` | | liveness check |
| `hatch-ble-bridge/clear_wifi` | in | forget WiFi, reboot into the setup AP |

The `nest/picker` and `hatch/rest` prefixes, the Hatch address and the sound delay can be changed
at runtime (see below).

## Build, flash, update

```bash
pio run -t upload                                        # first time, over USB
pio device monitor                                       # logs
pio run -t upload --upload-port hatch-ble-bridge.local   # later, over WiFi (OTA)
```

`platformio.ini` assumes 8 MB flash. On a 4 MB board, use `min_spiffs.csv`; the firmware fits at ~95%.

## Provisioning

This follows [axlan/pcb-map](https://github.com/axlan/pcb-map)'s scheme:

1. **WiFi:** on first boot (or after `clear-wifi-creds`), the board starts an access point
   called **`hatch-ble-bridge-ap`**. Join it and the captive portal asks for WiFi credentials,
   and optionally the MQTT broker.
2. **MQTT and bridge settings:** once on WiFi, the board advertises `_mqtt-config._udp` over mDNS
   and listens for plain-text UDP commands on port 5432. The setup tool lives in `../hatch-bridge`:

```bash
cd ../hatch-bridge
uv run hatch-ble-bridge-setup find-devices                        # mDNS → MQTT ping → setup-AP scan
uv run hatch-ble-bridge-setup status                              # WiFi / MQTT / Hatch status (JSON)
uv run hatch-ble-bridge-setup setup-mqtt -h 192.168.1.110         # broker (-p, -t, -u, -P)
uv run hatch-ble-bridge-setup setup-bridge --hatch-address e3:3f:db:c8:93:e2
uv run hatch-ble-bridge-setup setup-bridge --hatch-address auto   # first Hatch found
uv run hatch-ble-bridge-setup clear-wifi-creds                    # back to the setup AP
```

UDP protocol, if you'd rather use `nc -u`:

```
SET_MQTT host,port,use_tls(0/1),user,password                     -> OK   (same as pcb-map)
SET_BRIDGE picker_topic,hatch_topic,hatch_address,sound_delay_ms  -> OK   (empty address = first Hatch found)
GET_STATUS                                                        -> JSON (includes Hatches seen in the last scan)
FORGET_WIFI                                                       -> OK, then reboot into the setup AP
```

Settings are stored in NVS and survive reboots and OTA updates. Until a broker is configured,
the firmware uses `DEFAULT_MQTT_HOST` from `platformio.ini` (192.168.1.110).
