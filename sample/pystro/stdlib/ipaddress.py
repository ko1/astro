"""pystro stub for `ipaddress`.

CPython's ipaddress.py builds at module load `IPv6Network('fe80::/10')`
and similar fixtures, which exposes a deep pystro attribute-access
specialization quirk (only repros inside ipaddress.py's
__slots__-heavy class layout).  Bundle a lightweight stub that
provides the public surface used by urllib.parse / shutil / pathlib
chains without instantiating the IPv6Network constants at import.
"""


class AddressValueError(ValueError):
    pass


class NetmaskValueError(ValueError):
    pass


def _ip_int_from_string_v4(s):
    parts = s.split(".")
    if len(parts) != 4:
        raise AddressValueError(f"invalid IPv4: {s!r}")
    n = 0
    for p in parts:
        if not p.isdigit():
            raise AddressValueError(f"invalid IPv4: {s!r}")
        v = int(p)
        if v < 0 or v > 255:
            raise AddressValueError(f"octet out of range: {p!r}")
        n = (n << 8) | v
    return n


def _ip_int_from_bytes_be(b):
    n = 0
    for x in b:
        n = (n << 8) | x
    return n


class _BaseAddress:
    def __init__(self, address):
        if isinstance(address, int):
            self._ip = address
        elif isinstance(address, (bytes, bytearray)):
            self._ip = _ip_int_from_bytes_be(address)
        else:
            self._ip = self._parse(str(address))

    def __int__(self): return self._ip
    def __eq__(self, other):
        try: return self._ip == other._ip and self._version == other._version
        except AttributeError: return NotImplemented
    def __hash__(self): return hash((self._version, self._ip))
    def __repr__(self): return f"{type(self).__name__}({str(self)!r})"


class IPv4Address(_BaseAddress):
    _version = 4
    _max_prefixlen = 32
    @staticmethod
    def _parse(s):
        return _ip_int_from_string_v4(s)
    def __str__(self):
        x = self._ip
        return ".".join(str((x >> (8 * (3 - i))) & 0xFF) for i in range(4))
    @property
    def packed(self):
        x = self._ip
        return bytes(((x >> (8 * (3 - i))) & 0xFF) for i in range(4))


class IPv6Address(_BaseAddress):
    _version = 6
    _max_prefixlen = 128

    def __init__(self, address):
        # Scope id: `fe80::1%eth0` — split on `%`, parse rest, store scope.
        self._scope_id = None
        if isinstance(address, str) and "%" in address:
            addr, sep, scope = address.partition("%")
            if not scope or "%" in scope:
                raise AddressValueError(f"Invalid IPv6 address: {address!r}")
            self._scope_id = scope
            address = addr
        super().__init__(address)

    @property
    def scope_id(self):
        return self._scope_id

    @staticmethod
    def _parse(s):
        # Very minimal: handle "::", "1::", "::1", or hex with colons.
        if "::" in s:
            head, tail = s.split("::", 1)
            head_parts = head.split(":") if head else []
            tail_parts = tail.split(":") if tail else []
            missing = 8 - len(head_parts) - len(tail_parts)
            parts = head_parts + ["0"] * missing + tail_parts
        else:
            parts = s.split(":")
        if len(parts) != 8:
            raise AddressValueError(f"invalid IPv6: {s!r}")
        n = 0
        for p in parts:
            n = (n << 16) | int(p or "0", 16)
        return n
    def __str__(self):
        x = self._ip
        parts = [(x >> (16 * (7 - i))) & 0xFFFF for i in range(8)]
        s = ":".join(f"{p:x}" for p in parts)
        if self._scope_id:
            s += "%" + self._scope_id
        return s
    @property
    def packed(self):
        x = self._ip
        return bytes(((x >> (8 * (15 - i))) & 0xFF) for i in range(16))


class _BaseNetwork:
    def __init__(self, address, strict=True):
        if isinstance(address, str):
            if "/" in address:
                addr, prefix = address.split("/", 1)
                self._prefixlen = int(prefix)
            else:
                addr = address
                self._prefixlen = self._max_prefixlen
            self.network_address = self._address_class(addr)
        elif isinstance(address, tuple):
            self.network_address = self._address_class(address[0])
            self._prefixlen = int(address[1]) if len(address) > 1 else self._max_prefixlen
        else:
            self.network_address = self._address_class(address)
            self._prefixlen = self._max_prefixlen
        self._strict = strict
    @property
    def prefixlen(self): return self._prefixlen
    @property
    def netmask(self):
        all_ones = (1 << self._max_prefixlen) - 1
        return self._address_class(all_ones ^ (all_ones >> self._prefixlen))
    @property
    def broadcast_address(self):
        all_ones = (1 << self._max_prefixlen) - 1
        host_mask = all_ones >> self._prefixlen
        return self._address_class(int(self.network_address) | host_mask)
    @property
    def hostmask(self):
        all_ones = (1 << self._max_prefixlen) - 1
        return self._address_class(all_ones >> self._prefixlen)
    def __str__(self): return f"{self.network_address}/{self._prefixlen}"
    def __repr__(self): return f"{type(self).__name__}({str(self)!r})"
    def __eq__(self, other):
        try: return (self._version == other._version
                    and self.network_address == other.network_address
                    and self._prefixlen == other._prefixlen)
        except AttributeError: return NotImplemented
    def __hash__(self): return hash((self._version, int(self.network_address), self._prefixlen))
    def __contains__(self, other):
        if isinstance(other, _BaseAddress):
            return int(other) & int(self.netmask) == int(self.network_address)
        return False


class IPv4Network(_BaseNetwork):
    _version = 4
    _max_prefixlen = 32
    _address_class = IPv4Address


class IPv6Network(_BaseNetwork):
    _version = 6
    _max_prefixlen = 128
    _address_class = IPv6Address


class IPv4Interface(IPv4Address):
    def __init__(self, address):
        if isinstance(address, str) and "/" in address:
            addr, prefix = address.split("/", 1)
            super().__init__(addr)
            self._prefixlen = int(prefix)
        else:
            super().__init__(address)
            self._prefixlen = self._max_prefixlen
        self.network = IPv4Network((str(self), self._prefixlen), strict=False)


class IPv6Interface(IPv6Address):
    def __init__(self, address):
        if isinstance(address, str) and "/" in address:
            addr, prefix = address.split("/", 1)
            super().__init__(addr)
            self._prefixlen = int(prefix)
        else:
            super().__init__(address)
            self._prefixlen = self._max_prefixlen
        self.network = IPv6Network((str(self), self._prefixlen), strict=False)


def ip_address(address):
    if isinstance(address, int):
        if address > 0xFFFFFFFF:
            return IPv6Address(address)
        return IPv4Address(address)
    if isinstance(address, str):
        if ":" in address: return IPv6Address(address)
        return IPv4Address(address)
    return IPv4Address(address)


def ip_network(address, strict=True):
    if isinstance(address, str):
        if ":" in address: return IPv6Network(address, strict)
        return IPv4Network(address, strict)
    if isinstance(address, int):
        if address > 0xFFFFFFFF: return IPv6Network(address, strict)
        return IPv4Network(address, strict)
    return IPv4Network(address, strict)


def ip_interface(address):
    if isinstance(address, str) and ":" in address.split("/", 1)[0]:
        return IPv6Interface(address)
    return IPv4Interface(address)


def v4_int_to_packed(x): return IPv4Address(x).packed
def v6_int_to_packed(x): return IPv6Address(x).packed


def collapse_addresses(addresses):
    return list(addresses)


def get_mixed_type_key(obj):
    return (obj._version, obj._ip)


def summarize_address_range(first, last):
    return [ip_network((int(first), 32))]


# CPython exposes these for testing.
IPV4LENGTH = 32
IPV6LENGTH = 128


__all__ = [
    "IPv4Address", "IPv6Address", "IPv4Network", "IPv6Network",
    "IPv4Interface", "IPv6Interface",
    "ip_address", "ip_network", "ip_interface",
    "AddressValueError", "NetmaskValueError",
    "v4_int_to_packed", "v6_int_to_packed",
    "collapse_addresses", "get_mixed_type_key", "summarize_address_range",
    "IPV4LENGTH", "IPV6LENGTH",
]
