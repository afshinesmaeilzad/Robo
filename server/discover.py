"""Finding the robot on the network.

The robot is the one being found: the server always opens the connection, so
nothing on the robot needs to know this machine's address.

Three ways, in order of effort:
  * ROBOT_HOST is an address or name  -> used as it is;
  * "robo.local"                      -> mDNS, which works on the host but not
                                         inside most Docker containers;
  * "auto"                            -> ask every address on the local network
                                         whether it answers /info like a Robo.

The scan is 254 requests with a short timeout, run in parallel: a second or two.
"""

from __future__ import annotations

import asyncio
import ipaddress
import socket

import httpx


def local_networks() -> list[ipaddress.IPv4Network]:
    """The /24s this machine sits on, best guess, most likely first."""
    nets: list[ipaddress.IPv4Network] = []
    try:
        # No traffic is sent; this just picks the interface with the default route
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        nets.append(ipaddress.ip_network(f"{ip}/24", strict=False))
    except OSError:
        pass
    for extra in ("192.168.4.0/24",):  # the robot's own access point
        net = ipaddress.ip_network(extra)
        if net not in nets:
            nets.append(net)
    return nets


async def _is_robot(client: httpx.AsyncClient, host: str) -> bool:
    try:
        r = await client.get(f"http://{host}/info", timeout=0.8)
        data = r.json()
    except Exception:  # noqa: BLE001 - nearly every address will refuse or time out
        return False
    return isinstance(data, dict) and "cam" in data and "up" in data


async def find_robot(hint: str | None = None, concurrency: int = 64) -> str | None:
    """Return the robot's address, or None. Tries the hint and mDNS first."""
    async with httpx.AsyncClient() as client:
        for candidate in [hint, "robo.local", "192.168.4.1"]:
            if candidate and await _is_robot(client, candidate):
                return candidate

        for net in local_networks():
            hosts = [str(h) for h in net.hosts()]
            sem = asyncio.Semaphore(concurrency)
            found: list[str] = []

            async def check(host: str) -> None:
                async with sem:
                    if found:
                        return
                    if await _is_robot(client, host):
                        found.append(host)

            await asyncio.gather(*(check(h) for h in hosts))
            if found:
                return found[0]
    return None
