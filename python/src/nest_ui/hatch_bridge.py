"""MQTT -> Hatch Rest (gen 1) BLE bridge.

Subscribes to the color and sound the Nest picker publishes and applies them to
a Hatch Rest over a single persistent BLE connection. It only ever changes the
color and the sound: it never powers the Hatch on/off, and never touches
brightness or volume (color is sent with the Hatch's current brightness).
Color/sound commands sent while the Hatch is off are stored by the Hatch
without turning it on.

Hatch Rest gen 1 protocol (same as pyhatchbabyrest):
  write ASCII to 02240002-5efd-47eb-9c1a-de53f7a2b232
    SCrrggbbLL  color + brightness (hex)    SNxx  sound id (hex)
  read/notify 02260002-5efd-47eb-9c1a-de53f7a2b232
    [5]='C' [6:10]=r,g,b,brightness  [10]='S' [11]=sound [12]=volume
    [13]='P' [14]=flags (power is off when flags & 0xC0)
"""

from __future__ import annotations

import argparse
import asyncio
import colorsys
import json
import logging
import os
import signal
from dataclasses import dataclass

import aiomqtt
from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError

CHAR_TX = "02240002-5efd-47eb-9c1a-de53f7a2b232"
CHAR_FEEDBACK = "02260002-5efd-47eb-9c1a-de53f7a2b232"
HATCH_MANUFACTURER_ID = 1076
GRADIENT_RGB = (254, 254, 254)  # magic value: puts the Hatch into gradient mode

# Hatch Rest gen 1 sound ids (pyhatchbabyrest names)
SOUNDS = {"none": 0, "stream": 2, "noise": 3, "dryer": 4, "ocean": 5, "wind": 6, "rain": 7,
          "bird": 9, "crickets": 10, "brahms": 11, "twinkle": 13, "rockabye": 14}

log = logging.getLogger("hatch-bridge")


@dataclass
class HatchState:
    rgb: tuple[int, int, int]
    brightness: int
    sound: int
    volume: int
    power: bool

    @classmethod
    def parse(cls, data: bytes) -> "HatchState | None":
        if len(data) < 15 or data[5] != ord("C") or data[10] != ord("S") or data[13] != ord("P"):
            return None
        return cls(rgb=(data[6], data[7], data[8]), brightness=data[9], sound=data[11],
                   volume=data[12], power=not (data[14] & 0xC0))

    def as_json(self) -> str:
        r, g, b = self.rgb
        return json.dumps({
            "power": self.power, "hex": f"#{r:02x}{g:02x}{b:02x}", "r": r, "g": g, "b": b,
            "brightness": self.brightness, "sound": self.sound, "volume": self.volume,
        })


def _json(payload: bytes):
    try:
        return json.loads(payload)
    except (ValueError, UnicodeDecodeError):
        log.warning("ignoring non-JSON payload: %r", payload[:80])
        return None


def rgb_from_payload(payload: bytes) -> tuple[int, int, int] | None:
    """Picker color JSON -> full-intensity RGB (brightness is left to the Hatch)."""
    msg = _json(payload)
    if not isinstance(msg, dict):
        return None
    if "h" in msg:
        s = max(0.0, min(float(msg.get("s", 100)), 100.0)) / 100
        r, g, b = (round(c * 255) for c in colorsys.hsv_to_rgb((float(msg["h"]) % 360) / 360, s, 1.0))
    elif all(k in msg for k in ("r", "g", "b")):
        r, g, b = int(msg["r"]), int(msg["g"]), int(msg["b"])
        peak = max(r, g, b)
        if peak:
            r, g, b = (round(c * 255 / peak) for c in (r, g, b))
    else:
        log.warning("color payload has neither h nor r/g/b: %r", msg)
        return None
    if (r, g, b) == GRADIENT_RGB:
        r = 255
    return r, g, b


def sound_from_payload(payload: bytes) -> int | None:
    """Picker sound JSON ({"id":3,"name":"noise"}), a bare id, or a name -> Hatch sound id."""
    text = payload.decode("utf-8", "replace").strip()
    if text.lower() in SOUNDS:
        return SOUNDS[text.lower()]
    msg = _json(payload)
    if isinstance(msg, int):
        return msg
    if isinstance(msg, dict):
        if "id" in msg:
            return int(msg["id"])
        if str(msg.get("name", "")).lower() in SOUNDS:
            return SOUNDS[msg["name"].lower()]
    log.warning("unrecognised sound payload: %r", payload[:80])
    return None


class Hatch:
    """Keeps one BLE connection open and pushes only the latest requested color/sound."""

    def __init__(self, address: str | None, sound_delay: float, on_state):
        self.address = address
        self.sound_delay = sound_delay
        self.on_state = on_state
        self.client: BleakClient | None = None
        self.state: HatchState | None = None
        self._color: tuple[int, int, int] | None = None
        self._sound: int | None = None
        self._sound_due = 0.0
        self._wake = asyncio.Event()
        self._disconnected = asyncio.Event()

    def request_color(self, rgb: tuple[int, int, int]) -> None:
        self._color = rgb
        self._wake.set()

    def request_sound(self, sound: int) -> None:
        # Debounced: scrolling through sounds on the Nest shouldn't play each one.
        self._sound = sound
        self._sound_due = asyncio.get_running_loop().time() + self.sound_delay
        self._wake.set()

    async def _find(self):
        if self.address:
            dev = await BleakScanner.find_device_by_address(self.address, timeout=20)
        else:
            dev = await BleakScanner.find_device_by_filter(
                lambda d, adv: HATCH_MANUFACTURER_ID in adv.manufacturer_data, timeout=20)
        if dev is None:
            raise BleakError(f"Hatch {self.address or '(any)'} not found in scan")
        if not self.address:
            log.info("found Hatch %s (%s)", dev.address, dev.name)
            self.address = dev.address
        return dev

    def _feedback(self, _char, data: bytearray) -> None:
        st = HatchState.parse(bytes(data))
        if st and st != self.state:
            self.state = st
            log.debug("state %s", st)
            self.on_state(st)

    async def _connect(self) -> None:
        dev = await self._find()
        self._disconnected.clear()
        # Gen 1 Hatches can take ~20 s to accept a connection at weak signal.
        client = BleakClient(dev, timeout=45, disconnected_callback=lambda _c: self._disconnected.set())
        await client.connect()
        self.client = client
        self._feedback(None, await client.read_gatt_char(CHAR_FEEDBACK))
        try:
            await client.start_notify(CHAR_FEEDBACK, self._feedback)
        except BleakError as e:
            log.info("feedback notifications unavailable (%s); state will only be read on connect", e)
        log.info("connected to Hatch %s (%s)", dev.address, dev.name)

    async def _send(self, cmd: str) -> None:
        assert self.client
        log.debug("-> %s", cmd)
        await self.client.write_gatt_char(CHAR_TX, cmd.encode("ascii"), response=True)

    async def _service(self) -> None:
        """Send whatever is pending and due."""
        loop = asyncio.get_running_loop()
        if self._color is not None:
            (r, g, b), self._color = self._color, None
            brightness = self.state.brightness if self.state else 0x7F
            await self._send(f"SC{r:02x}{g:02x}{b:02x}{brightness:02x}")
            if self.state:
                self.state.rgb = (r, g, b)
        if self._sound is not None and loop.time() >= self._sound_due:
            sound, self._sound = self._sound, None
            if not self.state or self.state.sound != sound:
                await self._send(f"SN{sound:02x}")

    async def run(self) -> None:
        loop = asyncio.get_running_loop()
        backoff = 2
        while True:
            try:
                await self._connect()
                backoff = 2
                while not self._disconnected.is_set():
                    await self._service()
                    timeout = None
                    if self._sound is not None:
                        timeout = max(0.0, self._sound_due - loop.time())
                    if self._color is not None:
                        timeout = 0
                    wake = asyncio.create_task(self._wake.wait())
                    lost = asyncio.create_task(self._disconnected.wait())
                    await asyncio.wait({wake, lost}, timeout=timeout, return_when=asyncio.FIRST_COMPLETED)
                    wake.cancel()
                    lost.cancel()
                    self._wake.clear()
                log.warning("Hatch disconnected")
            except (BleakError, asyncio.TimeoutError, OSError) as e:
                log.warning("Hatch connection problem: %r; retrying in %ss", e, backoff)
                await asyncio.sleep(backoff)
                backoff = min(backoff * 2, 60)
            finally:
                if self.client:
                    try:
                        await self.client.disconnect()
                    except Exception:  # noqa: BLE001 - best-effort cleanup
                        pass
                    self.client = None


async def bridge(args) -> None:
    loop = asyncio.get_running_loop()
    color_topic = f"{args.topic}/color"
    sound_topic = f"{args.topic}/sound"
    state_topic = f"{args.hatch_topic}/state"
    status_topic = f"{args.hatch_topic}/status"
    mqtt_client: aiomqtt.Client | None = None

    def publish_state(st: HatchState) -> None:
        if mqtt_client is not None:
            loop.create_task(mqtt_client.publish(state_topic, st.as_json(), retain=True))

    hatch = Hatch(args.hatch, sound_delay=args.sound_delay, on_state=publish_state)
    hatch_task = asyncio.create_task(hatch.run())

    will = aiomqtt.Will(status_topic, "offline", retain=True)
    backoff = 2
    try:
        while True:
            try:
                async with aiomqtt.Client(args.broker, port=args.port, username=args.user or None,
                                          password=args.password or None, will=will,
                                          identifier=args.client_id) as client:
                    mqtt_client = client
                    backoff = 2
                    await client.publish(status_topic, "online", retain=True)
                    if hatch.state:
                        publish_state(hatch.state)
                    await client.subscribe(color_topic)
                    await client.subscribe(sound_topic)
                    log.info("listening on %s:%s %s, %s", args.broker, args.port, color_topic, sound_topic)
                    async for msg in client.messages:
                        topic = str(msg.topic)
                        if topic == color_topic:
                            rgb = rgb_from_payload(msg.payload)
                            if rgb:
                                log.info("color #%02x%02x%02x%s", *rgb, " (retained)" if msg.retain else "")
                                hatch.request_color(rgb)
                        elif topic == sound_topic:
                            sound = sound_from_payload(msg.payload)
                            if sound is not None:
                                log.info("sound %d%s", sound, " (retained)" if msg.retain else "")
                                hatch.request_sound(sound)
            except aiomqtt.MqttError as e:
                mqtt_client = None
                log.warning("MQTT error: %s; reconnecting in %ss", e, backoff)
                await asyncio.sleep(backoff)
                backoff = min(backoff * 2, 60)
    finally:
        hatch_task.cancel()


def main() -> None:
    env = os.environ.get
    p = argparse.ArgumentParser(description="Bridge Nest picker color + sound to a Hatch Rest (gen 1) over BLE")
    p.add_argument("--broker", default=env("MQTT_HOST", "192.168.1.110"))
    p.add_argument("--port", type=int, default=int(env("MQTT_PORT", "1883")))
    p.add_argument("--user", default=env("MQTT_USER"))
    p.add_argument("--password", default=env("MQTT_PASS"))
    p.add_argument("--client-id", default=env("MQTT_CLIENT_ID", "hatch-bridge"))
    p.add_argument("--topic", default=env("PICKER_TOPIC", "nest/picker"),
                   help="picker topic prefix; reads <prefix>/color and <prefix>/sound (default: %(default)s)")
    p.add_argument("--hatch", default=env("HATCH_ADDRESS"),
                   help="Hatch BLE address; omit to use the first Hatch found")
    p.add_argument("--hatch-topic", default=env("HATCH_TOPIC", "hatch/rest"),
                   help="prefix for <prefix>/state and <prefix>/status (default: %(default)s)")
    p.add_argument("--sound-delay", type=float, default=float(env("SOUND_DELAY", "0.6")),
                   help="seconds a sound must stay selected before it is sent (default: %(default)s)")
    p.add_argument("-v", "--verbose", action="store_true")
    args = p.parse_args()

    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)s %(message)s")
    logging.getLogger("bleak").setLevel(logging.INFO if args.verbose else logging.WARNING)

    async def runner():
        task = asyncio.create_task(bridge(args))
        loop = asyncio.get_running_loop()
        for sig in (signal.SIGINT, signal.SIGTERM):
            loop.add_signal_handler(sig, task.cancel)
        try:
            await task
        except asyncio.CancelledError:
            log.info("stopped")

    asyncio.run(runner())


if __name__ == "__main__":
    main()
