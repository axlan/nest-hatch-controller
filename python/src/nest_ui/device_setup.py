"""Discover and configure the hatch-ble-bridge ESP32 (see ../hatch-ble-bridge).

Modeled on axlan/pcb-map python/pcb_map/device_setup.py:
  find-devices      mDNS (_mqtt-config._udp) -> MQTT ping/pong -> setup-AP scan
  setup-mqtt        UDP "SET_MQTT host,port,tls,user,pass"
  setup-bridge      UDP "SET_BRIDGE picker_topic,hatch_topic,hatch_address,sound_delay_ms"
  status            UDP "GET_STATUS" (JSON)
  clear-wifi-creds  forget WiFi and reboot into the hatch-ble-bridge-ap portal
"""

from __future__ import annotations

import json
import shutil
import socket
import subprocess
import time
from typing import Annotated

import paho.mqtt.client as mqtt
import typer
from zeroconf import ServiceBrowser, ServiceStateChange, Zeroconf

BASE_NAME = "hatch-ble-bridge"
MDNS_HOSTNAME = BASE_NAME + ".local"
AP_SSID = BASE_NAME + "-ap"
MDNS_SERVICE_TYPE = "_mqtt-config._udp.local."
UDP_CONFIG_PORT = 5432
MQTT_PING_TOPIC = BASE_NAME + "/ping"
MQTT_PONG_TOPIC = BASE_NAME + "/pong"
MQTT_CLEAR_WIFI_TOPIC = BASE_NAME + "/clear_wifi"
DEFAULT_BROKER = "192.168.1.110"

MQTTHostnameOption = Annotated[str, typer.Option("--mqtt-hostname", "-h", help="MQTT broker hostname", envvar="MQTT_HOSTNAME")]
MQTTPortOption = Annotated[int, typer.Option("--mqtt-port", "-p", help="MQTT broker port. Default based on '--mqtt-use-tls'", show_default="1883/8883", envvar="MQTT_PORT")]
MQTTUseTlsOption = Annotated[bool, typer.Option("--mqtt-use-tls", "-t", help="Use TLS for connection", envvar="MQTT_USE_TLS")]
MQTTUsernameOption = Annotated[str, typer.Option("--mqtt-username", "-u", help="MQTT broker username", envvar="MQTT_USERNAME")]
MQTTPasswordOption = Annotated[str, typer.Option("--mqtt-password", "-P", help="MQTT broker password", envvar="MQTT_PASSWORD")]
DeviceOption = Annotated[str, typer.Option("--device", "-d", help="Device IP/hostname (default: find via mDNS)", envvar="HATCH_BLE_BRIDGE")]

app = typer.Typer(help="Discover and configure the hatch-ble-bridge ESP32.", add_completion=False)


class State:
    verbose = False


state = State()


@app.callback()
def main(verbose: bool = typer.Option(False, "--verbose", "-v", help="Enable verbose output")):
    state.verbose = verbose


def get_port(port_arg: int, use_tls: bool) -> int:
    return port_arg or (8883 if use_tls else 1883)


# --------------------------------------------------------------------------- discovery

def mdns_find(timeout: float = 3.0) -> list[tuple[str, str]]:
    """Return [(service name, IPv4)] for hatch-ble-bridge devices advertising the config service."""
    found: dict[str, str] = {}

    def on_change(zeroconf: Zeroconf, service_type: str, name: str, state_change: ServiceStateChange) -> None:
        if state_change is not ServiceStateChange.Added or BASE_NAME not in name.lower():
            return
        info = zeroconf.get_service_info(service_type, name, timeout=2000)
        addrs = info.parsed_addresses() if info else []
        ipv4 = next((a for a in addrs if ":" not in a), addrs[0] if addrs else "")
        found[name] = ipv4

    zc = Zeroconf()
    try:
        ServiceBrowser(zc, MDNS_SERVICE_TYPE, handlers=[on_change])
        time.sleep(timeout)
    finally:
        zc.close()
    return list(found.items())


def resolve_device(device: str) -> str:
    if device:
        return device
    devices = mdns_find()
    if devices:
        name, ip = devices[0]
        if state.verbose:
            typer.echo(f"Using {name} at {ip}")
        if ip:
            return ip
    return MDNS_HOSTNAME  # fall back to the system resolver (nss-mdns / Avahi)


def udp_command(device: str, message: str, timeout: float = 3.0) -> str:
    host = resolve_device(device)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.settimeout(timeout)
        sock.sendto(message.encode(), (host, UDP_CONFIG_PORT))
        try:
            data, _ = sock.recvfrom(4096)
        except socket.timeout:
            typer.echo(f"✗ No reply from {host}:{UDP_CONFIG_PORT}", err=True)
            raise typer.Exit(1)
    return data.decode()


def mqtt_ping(hostname: str, port: int, username: str, password: str, use_tls: bool) -> bool:
    typer.echo(f"Pinging device over MQTT: {hostname}:{port}")
    got_pong = False

    def on_message(_c, _u, msg):
        nonlocal got_pong
        if msg.topic == MQTT_PONG_TOPIC:
            got_pong = True

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="hatch-ble-bridge-finder")
    if username:
        client.username_pw_set(username, password or None)
    if use_tls:
        client.tls_set()
    client.on_message = on_message
    try:
        client.connect(hostname, port, keepalive=10)
    except OSError as e:
        typer.echo(f"✗ Unable to connect to MQTT broker: {e}", err=True)
        return False
    client.loop_start()
    try:
        client.subscribe(MQTT_PONG_TOPIC)
        time.sleep(0.3)
        client.publish(MQTT_PING_TOPIC, "ping")
        deadline = time.time() + 3
        while time.time() < deadline and not got_pong:
            time.sleep(0.1)
    finally:
        client.loop_stop()
        client.disconnect()
    typer.echo("✓ Received pong from hatch-ble-bridge" if got_pong else "✗ hatch-ble-bridge did not respond to MQTT ping")
    return got_pong


def wifi_ap_scan() -> bool:
    """Look for the device's setup access point (needs NetworkManager's nmcli)."""
    typer.echo(f"Scanning for the {AP_SSID} setup access point")
    if not shutil.which("nmcli"):
        typer.echo("✗ nmcli not available; check your phone's WiFi list for " + AP_SSID)
        return False
    out = subprocess.run(["nmcli", "-t", "-f", "SSID,SIGNAL", "dev", "wifi", "list", "--rescan", "yes"],
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        ssid, _, signal = line.rpartition(":")
        if state.verbose:
            typer.echo(f"  {ssid} {signal}")
        if AP_SSID in ssid:
            typer.echo(f"✓ {AP_SSID} found ({signal}%). Connect to it and enter WiFi (and MQTT) settings.")
            return True
    typer.echo(f"✗ {AP_SSID} missing. Device powered off, out of range, or already on WiFi.")
    return False


# --------------------------------------------------------------------------- commands

@app.command()
def find_devices(
    mqtt_hostname: MQTTHostnameOption = DEFAULT_BROKER,
    mqtt_port: MQTTPortOption = 0,
    mqtt_use_tls: MQTTUseTlsOption = False,
    mqtt_username: MQTTUsernameOption = "",
    mqtt_password: MQTTPasswordOption = "",
    mqtt_only: Annotated[bool, typer.Option("--mqtt-only", help="Only check for the device over MQTT")] = False,
) -> None:
    """Find whether a hatch-ble-bridge is online (mDNS, then MQTT), else look for its setup AP."""
    mdns_found = False
    if not mqtt_only:
        typer.echo(f"Searching for {MDNS_SERVICE_TYPE} via mDNS...")
        devices = mdns_find()
        for name, ip in devices:
            typer.echo(f"✓ Found mDNS device: {name} at {ip}")
        mdns_found = bool(devices)
        if not mdns_found:
            typer.echo("✗ hatch-ble-bridge not found via mDNS.")
    if mqtt_only or mdns_found:
        mqtt_ping(mqtt_hostname, get_port(mqtt_port, mqtt_use_tls), mqtt_username, mqtt_password, mqtt_use_tls)
    else:
        wifi_ap_scan()


@app.command()
def status(device: DeviceOption = "") -> None:
    """Show the device's WiFi, MQTT and Hatch connection status."""
    reply = udp_command(device, "GET_STATUS")
    try:
        typer.echo(json.dumps(json.loads(reply), indent=2))
    except ValueError:
        typer.echo(reply)


@app.command()
def setup_mqtt(
    mqtt_hostname: MQTTHostnameOption = DEFAULT_BROKER,
    mqtt_port: MQTTPortOption = 0,
    mqtt_use_tls: MQTTUseTlsOption = False,
    mqtt_username: MQTTUsernameOption = "",
    mqtt_password: MQTTPasswordOption = "",
    device: DeviceOption = "",
) -> None:
    """Set the MQTT broker the device connects to."""
    if any("," in v for v in (mqtt_hostname, mqtt_username, mqtt_password)):
        typer.echo("',' not allowed in hostname, username or password", err=True)
        raise typer.Exit(1)
    port = get_port(mqtt_port, mqtt_use_tls)
    msg = f"SET_MQTT {mqtt_hostname},{port},{1 if mqtt_use_tls else 0},{mqtt_username},{mqtt_password}"
    typer.echo(udp_command(device, msg))


@app.command()
def setup_bridge(
    picker_topic: Annotated[str, typer.Option(help="Topic prefix the Nest picker publishes on (<prefix>/color, /sound)")] = "",
    hatch_topic: Annotated[str, typer.Option(help="Prefix for <prefix>/state and /status")] = "",
    hatch_address: Annotated[str, typer.Option(help="Hatch BLE address, or 'auto' for the first Hatch found")] = "",
    sound_delay_ms: Annotated[int, typer.Option(help="How long a sound must stay selected before it's sent")] = -1,
    device: DeviceOption = "",
) -> None:
    """Change bridge settings; anything not given keeps its current value."""
    current = json.loads(udp_command(device, "GET_STATUS"))["bridge"]
    picker = picker_topic or current["picker_topic"]
    hatch_t = hatch_topic or current["hatch_topic"]
    address = current["hatch_address"] if not hatch_address else ("" if hatch_address == "auto" else hatch_address.lower())
    delay = current["sound_delay_ms"] if sound_delay_ms < 0 else sound_delay_ms
    if any("," in v for v in (picker, hatch_t, address)):
        typer.echo("',' not allowed in values", err=True)
        raise typer.Exit(1)
    typer.echo(udp_command(device, f"SET_BRIDGE {picker},{hatch_t},{address},{delay}"))


@app.command()
def clear_wifi_creds(
    via_mqtt: Annotated[bool, typer.Option("--via-mqtt", help="Send over MQTT instead of UDP")] = False,
    mqtt_hostname: MQTTHostnameOption = DEFAULT_BROKER,
    mqtt_port: MQTTPortOption = 0,
    mqtt_use_tls: MQTTUseTlsOption = False,
    mqtt_username: MQTTUsernameOption = "",
    mqtt_password: MQTTPasswordOption = "",
    device: DeviceOption = "",
) -> None:
    """Forget the saved WiFi; the device reboots into the hatch-ble-bridge-ap portal."""
    if not via_mqtt:
        typer.echo(udp_command(device, "FORGET_WIFI"))
        return
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="hatch-ble-bridge-setup")
    if mqtt_username:
        client.username_pw_set(mqtt_username, mqtt_password or None)
    if mqtt_use_tls:
        client.tls_set()
    client.connect(mqtt_hostname, get_port(mqtt_port, mqtt_use_tls))
    client.loop_start()
    client.publish(MQTT_CLEAR_WIFI_TOPIC, b"").wait_for_publish(5)
    client.loop_stop()
    client.disconnect()
    typer.echo("sent")


if __name__ == "__main__":
    app()
