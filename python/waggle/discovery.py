# Copyright (c) The Einsums Developers. All rights reserved.
# Licensed under the MIT License. See LICENSE.txt in the project root for license information.

"""Finding profiler servers over mDNS, when the optional ``zeroconf`` package is installed.

Servers advertise through their platform's responder: Bonjour on macOS, the Avahi daemon on
Linux, and the DNS-SD service of Windows 10 1809 and later. A server on a machine with none of these
is not found here; the app always tries the default port as well.
"""

from __future__ import annotations

import socket
from collections.abc import Callable
from typing import Any

from .client import DEFAULT_HOST, MDNS_SERVICES

try:
    from zeroconf import ServiceBrowser, ServiceStateChange, Zeroconf

    HAVE_ZEROCONF = True
except ImportError:  # pragma: no cover - depends on the environment
    HAVE_ZEROCONF = False


def is_local_address(address: str) -> bool:
    """Whether *address* belongs to this machine: a socket binds only to an address it owns.

    The host name's addresses are not enough: a responder advertises on every interface, a
    container bridge's among them, and the host name resolves to none of those.
    """
    family = socket.AF_INET6 if ":" in address else socket.AF_INET
    try:
        with socket.socket(family, socket.SOCK_DGRAM) as probe:
            probe.bind((address, 0))
    except OSError:
        return False
    return True


def choose_host(addresses: list[str]) -> str | None:
    """Where to connect to a server advertised at *addresses*.

    A server on this machine listens on loopback, though its responder advertises it on every
    interface, so any address of this machine means loopback; otherwise the first address.
    """
    if not addresses:
        return None
    if any(is_local_address(a) for a in addresses):
        return DEFAULT_HOST
    return addresses[0]


class ServerBrowser:
    """Calls ``on_found(host, port, executable)`` from zeroconf's thread for each server seen."""

    def __init__(self, on_found: Callable[[str, int, str], None]) -> None:
        self._on_found = on_found
        self._zeroconf: Any = None
        self._browser: Any = None

    @property
    def running(self) -> bool:
        return self._zeroconf is not None

    def start(self) -> bool:
        if not HAVE_ZEROCONF or self.running:
            return self.running
        self._zeroconf = Zeroconf()
        self._browser = ServiceBrowser(self._zeroconf, list(MDNS_SERVICES), handlers=[self._on_change])
        return True

    def stop(self) -> None:
        if self._browser is not None:
            self._browser.cancel()
        if self._zeroconf is not None:
            self._zeroconf.close()
        self._browser = self._zeroconf = None

    def _on_change(self, zeroconf: Any, service_type: str, name: str, state_change: Any) -> None:
        if state_change not in (ServiceStateChange.Added, ServiceStateChange.Updated):
            return
        info = zeroconf.get_service_info(service_type, name)
        host = choose_host(info.parsed_addresses()) if info is not None else None
        if host is None:
            return
        props = {k.decode(): (v.decode() if v else "") for k, v in info.properties.items()}
        self._on_found(host, info.port, props.get("exe", "unknown"))
