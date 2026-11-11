"""MQTT -> Hatch Rest (gen 1) BLE bridge.

Subscribes to the color the Nest picker publishes (JSON with h/s/v) and drives a
Hatch Rest night light over a single persistent BLE connection.

Hatch Rest gen 1 protocol (same as pyhatchbabyrest):
  write ASCII to 02240002-5efd-47eb-9c1a-de53f7a2b232
    SCrrggbbLL  color + brightness (hex)   SI01 / SI00  power on / off
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

log = logging.getLogger("hatch-bridge")


@dataclass
class Target:
    """Desired light state (power False turns the Hatch off)."""
    rgb: tuple[int, int, int]
    brightness: int  # 0..255
    power: bool


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
        return cls(
            rgb=(data[6], data[7], data[8]),
            brightness=data[9],
            sound=data[11],
            volume=data[12],
            power=not (data[14] & 0xC0),
        )

    def as_json(self) -> str:
        r, g, b = self.rgb
        return json.dumps({
            "power": self.power, "hex": f"#{r:02x}{g:02x}{b:02x}", "r": r, "g": g, "b": b,
            "brightness": self.brightness, "sound": self.sound, "volume": self.volume,
        })


def target_from_payload(payload: bytes) -> Target | None:
    """Nest picker JSON -> Hatch target. Hue/sat give the color, v gives brightness."""
    try:
        msg = json.loads(payload)
    except (ValueError, UnicodeDecodeError):
        log.warning("ignoring non-JSON payload: %r", payload[:80])
        return None
    if all(k in msg for k in ("h", "s", "v")):
        h, s, v = float(msg["h"]), float(msg["s"]), float(msg["v"])
        r, g, b = (round(c * 255) for c in colorsys.hsv_to_rgb((h % 360) / 360, max(0, min(s, 100)) / 100, 1.0))
        brightness = round(max(0.0, min(v, 100.0)) * 255 / 100)
    elif all(k in msg for k in ("r", "g", "b")):
        r, g, b = int(msg["r"]), int(msg["g"]), int(msg["b"])
        peak = max(r, g, b)
        brightness = peak
        if peak:
            r, g, b = (round(c * 255 / peak) for c in (r, g, b))
    else:
        log.warning("payload has neither h/s/v nor r/g/b: %r", msg)
        return None
    if (r, g, b) == GRADIENT_RGB:
        r = 255
    return Target(rgb=(r, g, b), brightness=brightness, power=brightness > 0)


class Hatch:
    """Keeps one BLE connection open and pushes only the latest requested state."""

    def __init__(self, address: str | None, auto_power: bool, on_state):
        self.address = address
        self.auto_power = auto_power
        self.on_state = on_state
        self.client: BleakClient | None = None
        self.state: HatchState | None = None
        self._pending: Target | None = None
        self._wake = asyncio.Event()
        self._disconnected = asyncio.Event()

    def request(self, target: Target) -> None:
        self._pending = target
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

    async def _apply(self, t: Target) -> None:
        r, g, b = t.rgb
        if not t.power:
            if self.state is None or self.state.power:
                await self._send("SI00")
            return
        await self._send(f"SC{r:02x}{g:02x}{b:02x}{t.brightness:02x}")
        if self.auto_power and (self.state is None or not self.state.power):
            await self._send("SI01")
            if self.state:  # avoid re-sending SI01 until feedback says otherwise
                self.state.power = True

    async def run(self) -> None:
        backoff = 2
        while True:
            try:
                await self._connect()
                backoff = 2
                while not self._disconnected.is_set():
                    wake = asyncio.create_task(self._wake.wait())
                    lost = asyncio.create_task(self._disconnected.wait())
                    await asyncio.wait({wake, lost}, return_when=asyncio.FIRST_COMPLETED)
                    wake.cancel()
                    lost.cancel()
                    if self._disconnected.is_set():
                        break
                    self._wake.clear()
                    target, self._pending = self._pending, None
                    if target:
                        await self._apply(target)
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
    state_topic = f"{args.hatch_topic}/state"
    status_topic = f"{args.hatch_topic}/status"
    mqtt_client: aiomqtt.Client | None = None

    def publish_state(st: HatchState) -> None:
        if mqtt_client is not None:
            loop.create_task(mqtt_client.publish(state_topic, st.as_json(), retain=True))

    hatch = Hatch(args.hatch, auto_power=not args.no_power, on_state=publish_state)
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
                    await client.subscribe(args.topic)
                    log.info("listening on %s:%s %s", args.broker, args.port, args.topic)
                    async for msg in client.messages:
                        if msg.retain and not args.apply_retained:
                            log.info("skipping retained color from broker (use --apply-retained to apply it)")
                            continue
                        target = target_from_payload(msg.payload)
                        if target:
                            log.info("color %s brightness %d%s", "#%02x%02x%02x" % target.rgb,
                                     target.brightness, "" if target.power else " (off)")
                            hatch.request(target)
            except aiomqtt.MqttError as e:
                mqtt_client = None
                log.warning("MQTT error: %s; reconnecting in %ss", e, backoff)
                await asyncio.sleep(backoff)
                backoff = min(backoff * 2, 60)
    finally:
        hatch_task.cancel()


def main() -> None:
    env = os.environ.get
    p = argparse.ArgumentParser(description="Bridge Nest picker MQTT colors to a Hatch Rest (gen 1) over BLE")
    p.add_argument("--broker", default=env("MQTT_HOST", "192.168.1.110"))
    p.add_argument("--port", type=int, default=int(env("MQTT_PORT", "1883")))
    p.add_argument("--user", default=env("MQTT_USER"))
    p.add_argument("--password", default=env("MQTT_PASS"))
    p.add_argument("--client-id", default=env("MQTT_CLIENT_ID", "hatch-bridge"))
    p.add_argument("--topic", default=env("COLOR_TOPIC", "nest/picker/color"),
                   help="topic with the picker's JSON color (default: %(default)s)")
    p.add_argument("--hatch", default=env("HATCH_ADDRESS"),
                   help="Hatch BLE address; omit to use the first Hatch found")
    p.add_argument("--hatch-topic", default=env("HATCH_TOPIC", "hatch/rest"),
                   help="prefix for <prefix>/state and <prefix>/status (default: %(default)s)")
    p.add_argument("--apply-retained", action="store_true",
                   help="apply the broker's retained color at startup (off by default so restarts don't turn the light on)")
    p.add_argument("--no-power", action="store_true",
                   help="never send power on (SI01); only change color while the Hatch is already on")
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
