# nest-ui

Turns a rooted **Nest Learning Thermostat (Gen 1)** into a color and sound picker that
controls a **Hatch Rest (gen 1)** night light, over MQTT and Bluetooth LE.

```
 Nest Gen 1 (nest-picker)  ──MQTT──▶  broker  ──MQTT──▶  bridge  ──BLE──▶  Hatch Rest
 ring = hue / sound                                      ESP32-S3 (hatch-ble-bridge)
 press = switch page                                     or Python (hatch-bridge, for testing)
```

| Directory | What it is |
|---|---|
| [`src/`](src/), [`device/`](device/), [`build/`](build/), `build.sh`, `install.sh` | **nest-picker**: the Nest app (C, cross-compiled for the Nest's ARM/glibc 2.19) and its install scripts |
| [`hatch-ble-bridge/`](hatch-ble-bridge/) | **ESP32-S3 firmware** (PlatformIO): MQTT → Hatch bridge with WiFiManager/UDP provisioning and OTA |
| [`hatch-bridge/`](hatch-bridge/) | **Python tools** (uv): the same bridge in Python for testing, plus `hatch-ble-bridge-setup` to find and configure the ESP32 |

## Quick start

Requirements:
- A Nest Gen 1 rooted with [NoLongerEvil](https://github.com/codykociemba/NoLongerEvil-Thermostat), with SSH enabled and kept on USB power.
- An MQTT broker.
- Docker (to build the Nest app), PlatformIO (`uv tool install platformio`) and uv.

```bash
# 1. Nest: build, then install with your broker set in device/nest-picker.conf
./build.sh
./install.sh 192.168.1.112 --config

# 2. Bridge: flash the ESP32-S3, then join its "hatch-ble-bridge-ap" WiFi to set up WiFi
cd hatch-ble-bridge && pio run -t upload && cd ..
cd hatch-bridge
uv run hatch-ble-bridge-setup find-devices
uv run hatch-ble-bridge-setup setup-mqtt -h 192.168.1.110
uv run hatch-ble-bridge-setup status
```

For testing without the ESP32, run the Python bridge instead. Only one BLE client can be connected to the Hatch at a time.

```bash
cd hatch-bridge && uv run hatch-bridge --broker 192.168.1.110 --hatch E3:3F:DB:C8:93:E2
```

## MQTT topics

| Topic | From | Payload |
|---|---|---|
| `nest/picker/color` | Nest | `{"hex":"#ff8000","r":255,"g":128,"b":0,"h":30,"s":100,"v":100}` (retained) |
| `nest/picker/sound` | Nest | `{"id":3,"name":"noise"}`, using Hatch Rest sound ids (retained) |
| `nest/picker/status` | Nest | `online` / `offline` (retained, Last Will) |
| `hatch/rest/state` | bridge | `{"power","hex","r","g","b","brightness","sound","volume"}` (retained) |
| `hatch/rest/status` | bridge | `online` / `offline` (retained, Last Will) |
| `hatch-ble-bridge/ping` → `/pong`, `/clear_wifi` | ESP32 | liveness check; forget WiFi |

The bridges only ever change the Hatch's **color and sound**. They never turn it on or off and
never change brightness or volume. Color is sent at the Hatch's current brightness, and changes
made while the Hatch is off are stored for when it's next switched on.

---

## nest-picker (the Nest app)

- **Color page:** turn the ring to pick the hue. Saturation and brightness are fixed at 100%.
- **Press the display** to switch to the **sound page**. Turn the ring to step through None,
  Stream, White Noise, Dryer, Ocean, Wind, Rain, Crickets, Lullaby, Twinkle and Rockabye.
  Press again to go back.
- **Long-press** (≥0.7 s) to re-send the current color and sound ("SENT" flashes).
- Green or red dot at the bottom: MQTT connected or disconnected.
- Screen turns off after `SLEEP_SECS` of no input. Any touch wakes it, and that first touch doesn't change anything.
- The last color and sound are restored after a reboot.

Publishes are live while the ring turns, at most one per topic every `PUBLISH_MIN_MS`. The
topic prefix (`nest/picker`), broker, ring sensitivity (`HUE_STEP`, `SOUND_STEP`) and the rest
are set in [`device/nest-picker.conf`](device/nest-picker.conf).

Preview the screens on a PC without the device:
`gcc -std=gnu99 -O2 -o /tmp/np src/nest-picker.c -lm -lpthread && /tmp/np --render-test /tmp` writes PPM images.

### How it works

The stock UI (`nlclient`) and its software watchdog (`nlheartbeatd`) are frozen with
SIGSTOP. Killing them makes the device reboot; freezing them doesn't. This is the same trick
[nest-gallery](https://github.com/jasongraphix/nest-gallery) uses. With `nlclient` frozen, no
thermostat screens or wiring alerts can appear. The picker then draws straight to `/dev/fb0` and
reads `/dev/input/event1` (ring) and `/dev/input/event2` (press).

Boot: `/etc/init.d/rcS` starts `nest-picker-start`. It waits for Wi-Fi, then runs `nest-picker`
and restarts it if it exits. When the picker exits it resumes `nlclient`. Shutdown: a line at the
top of `/etc/init.d/rcH` stops the picker first. Without that, the frozen `nlclient` stalls
`reboot` for about 6 minutes.

Power: this assumes constant USB power. The shutdown manager logs "USB is present", so it never
powers the device down. Because `nlclient` is frozen, Wi-Fi power saving never kicks in.

### Build / install

```bash
./build.sh                              # Docker; Ubuntu 14.04 armhf cross-gcc (glibc 2.19 = device)
./install.sh 192.168.1.112 --config     # first install: push device/nest-picker.conf
./install.sh 192.168.1.112              # later updates (keeps config on device)
./install.sh 192.168.1.112 --uninstall  # back to the stock thermostat
```

Edit settings on the device in `/etc/nest-picker.conf`, then run `killall nest-picker`
(the supervisor restarts it). To pause the picker without uninstalling:
`touch /etc/nest-picker.disabled; killall nest-picker`.

Debug input: `nest-picker --evtest`. Logs: `grep nest-picker /var/log/messages`.

## Credits

- [NoLongerEvil-Thermostat](https://github.com/codykociemba/NoLongerEvil-Thermostat): Nest Gen 1 root access
- [nest-gallery](https://github.com/jasongraphix/nest-gallery): custom Nest app approach (freezing `nlclient`, framebuffer, input devices)
- [pyhatchbabyrest](https://github.com/kjoconnor/pyhatchbabyrest): Hatch Rest BLE protocol
- [pcb-map](https://github.com/axlan/pcb-map): ESP32 WiFi/MQTT provisioning scheme and setup tool
