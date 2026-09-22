"""Finding the robot on the network.

The robot is the one being found: the server always opens the connection, so
nothing on the robot needs to know this machine's address.

Three ways, in order of effort:
  * ROBOT_HOST is an address or name  -> used as it is;
  * "robo.local"                      -> mDNS, which works on the host but not
                                         inside most Docker containers;
  * "auto"                            -> ask every address on the local network
                                         whether it answers /info like a Robo.

The scan looks at the addresses this machine has recently talked to (the ARP
table) before trying a whole network, and it uses the network's real size: a
phone hotspot is a /28, so 14 addresses, not 254.
"""

from __future__ import annotations

import asyncio
import ipaddress
import socket
import subprocess

import httpx


def local_networks() -> list[ipaddress.IPv4Network]:
    """The networks this machine sits on, with their real size.

    The size matters: a phone hotspot hands out a /28 (14 addresses), and
    blasting 254 requests at one is both pointless and enough to make it drop
    the very reply we are waiting for.
    """
    nets: list[ipaddress.IPv4Network] = []
    try:
        out = subprocess.run(["ifconfig"], capture_output=True, text=True, timeout=5).stdout
        for line in out.splitlines():
            parts = line.split()
            if len(parts) >= 4 and parts[0] == "inet" and not parts[1].startswith("127."):
                ip = parts[1]
                mask = parts[3]
                if mask.startswith("0x"):  # macOS: inet 172.20.10.10 netmask 0xfffffff0
                    bits = bin(int(mask, 16)).count("1")
                elif "/" in ip:            # Linux ip-style
                    ip, bits = ip.split("/")
                    bits = int(bits)
                else:
                    continue
                nets.append(ipaddress.ip_network(f"{ip}/{bits}", strict=False))
    except (OSError, subprocess.SubprocessError, ValueError):
        pass
    if not nets:  # last resort: assume a /24 around our own address
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.connect(("8.8.8.8", 80))  # no traffic; picks the default interface
            nets.append(ipaddress.ip_network(f"{s.getsockname()[0]}/24", strict=False))
            s.close()
        except OSError:
            pass
    robot_ap = ipaddress.ip_network("192.168.4.0/24")  # the robot's own network
    if robot_ap not in nets:
        nets.append(robot_ap)
    return [n for n in nets if n.num_addresses <= 256]


def neighbours() -> list[str]:
    """Addresses this machine has recently talked to: a short, likely list."""
    try:
        out = subprocess.run(["arp", "-an"], capture_output=True, text=True, timeout=5).stdout
    except (OSError, subprocess.SubprocessError):
        return []
    found = []
    for line in out.splitlines():
        start = line.find("(")
        end = line.find(")")
        if start >= 0 < end:
            ip = line[start + 1 : end]
            try:
                ipaddress.ip_address(ip)
                found.append(ip)
            except ValueError:
                pass
    return found


async def _is_robot(client: httpx.AsyncClient, host: str) -> bool:
    try:
        r = await client.get(f"http://{host}/info", timeout=1.5)
        data = r.json()
    except Exception:  # noqa: BLE001 - nearly every address will refuse or time out
        return False
    return isinstance(data, dict) and "cam" in data and "up" in data


async def _first_robot(client: httpx.AsyncClient, hosts: list[str], concurrency: int) -> str | None:
    sem = asyncio.Semaphore(concurrency)
    found: list[str] = []

    async def check(host: str) -> None:
        async with sem:
            if found:
                return
            if await _is_robot(client, host):
                found.append(host)

    await asyncio.gather(*(check(h) for h in hosts))
    return found[0] if found else None


async def find_robot(hint: str | None = None, concurrency: int = 16) -> str | None:
    """Return the robot's address, or None. Tries the cheap guesses first."""
    async with httpx.AsyncClient() as client:
        for candidate in [hint, "robo.local", "192.168.4.1"]:
            if candidate and await _is_robot(client, candidate):
                return candidate

        # Neighbours first: a handful of addresses instead of a whole network
        seen = {hint, "robo.local", "192.168.4.1"}
        near = [h for h in neighbours() if h not in seen]
        if near and (found := await _first_robot(client, near, concurrency)):
            return found
        seen.update(near)

        for net in local_networks():
            hosts = [str(h) for h in net.hosts() if str(h) not in seen]
            if found := await _first_robot(client, hosts, concurrency):
                return found
    return None
