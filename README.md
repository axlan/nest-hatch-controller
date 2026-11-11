# nest-picker

Turns a rooted Nest Learning Thermostat (Gen 1) into an MQTT color picker.

- **Turn the ring** to change the active channel.
- **Press the display** to cycle Hue → Saturation → Brightness.
- **Long-press** (≥0.7 s) to re-send the current color ("SENT" flashes).
- Green or red dot at the bottom: MQTT connected or disconnected.
- Screen turns off after `SLEEP_SECS` of no input. Any touch wakes it, and that first touch doesn't change anything.

## MQTT

| Topic | Payload |
|---|---|
| `<MQTT_TOPIC>/color` | `{"hex":"#ff8000","r":255,"g":128,"b":0,"h":30,"s":100,"v":100}` (retained by default) |
| `<MQTT_TOPIC>/status` | `online` / `offline` (retained, `offline` is the Last Will) |

Publishes are live while the ring turns, at most one every `PUBLISH_MIN_MS`.

## How it works

The stock UI (`nlclient`) and its software watchdog (`nlheartbeatd`) are frozen with
SIGSTOP. Killing them makes the device reboot; freezing them doesn't (this is the
same trick nest-gallery uses). With `nlclient` frozen, no thermostat screens or
wiring alerts can appear. The picker then draws straight to `/dev/fb0` and reads
`/dev/input/event1` (ring) and `/dev/input/event2` (press).

Boot: `/etc/init.d/rcS` starts `nest-picker-start`. It waits for Wi-Fi, then runs
`nest-picker` and restarts it if it exits. When the picker exits it resumes
`nlclient`. Shutdown: a line at the top of `/etc/init.d/rcH` stops the picker first.
Without that, the frozen `nlclient` stalls `reboot` for about 6 minutes.

Power: this assumes constant USB power. The shutdown manager logs "USB is
present", so it never powers the device down. Because `nlclient` is frozen, Wi-Fi
power saving never kicks in.

## Build / install

```bash
./build.sh                              # Docker; Ubuntu 14.04 armhf cross-gcc (glibc 2.19 = device)
./install.sh 192.168.1.112 --config     # first install: push device/nest-picker.conf
./install.sh 192.168.1.112              # later updates (keeps config on device)
./install.sh 192.168.1.112 --uninstall  # back to the stock thermostat
```

Edit settings on the device in `/etc/nest-picker.conf`, then `killall nest-picker`
(the supervisor restarts it). To pause the picker without uninstalling:
`touch /etc/nest-picker.disabled; killall nest-picker`.

Debug input: `nest-picker --evtest`. Logs: `grep nest-picker /var/log/messages`.
