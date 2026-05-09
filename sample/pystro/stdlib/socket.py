"""pystro stub for `socket`.  pystro is single-process / single-thread
without networking; this module exposes the surface CPython tests
typically interrogate (constants, error class, simple socket-with-no-OP
behaviour) so import + isinstance checks succeed."""


class error(OSError):
    pass


# Address families
AF_UNSPEC = 0
AF_UNIX = 1
AF_INET = 2
AF_INET6 = 10
AF_NETLINK = 16
AF_PACKET = 17
AF_BLUETOOTH = 31

# Socket types
SOCK_STREAM = 1
SOCK_DGRAM = 2
SOCK_RAW = 3
SOCK_RDM = 4
SOCK_SEQPACKET = 5
SOCK_NONBLOCK = 0x800
SOCK_CLOEXEC = 0x80000

# Protocols
IPPROTO_IP = 0
IPPROTO_ICMP = 1
IPPROTO_TCP = 6
IPPROTO_UDP = 17
IPPROTO_IPV6 = 41
IPPROTO_RAW = 255

# SOL_*
SOL_SOCKET = 1
SOL_TCP = 6
SOL_UDP = 17

# SO_*
SO_DEBUG = 1
SO_REUSEADDR = 2
SO_TYPE = 3
SO_ERROR = 4
SO_DONTROUTE = 5
SO_BROADCAST = 6
SO_SNDBUF = 7
SO_RCVBUF = 8
SO_KEEPALIVE = 9
SO_OOBINLINE = 10
SO_LINGER = 13
SO_REUSEPORT = 15
SO_RCVLOWAT = 18
SO_SNDLOWAT = 19
SO_RCVTIMEO = 20
SO_SNDTIMEO = 21
SO_ACCEPTCONN = 30

# MSG_*
MSG_OOB = 1
MSG_PEEK = 2
MSG_DONTROUTE = 4
MSG_CTRUNC = 8
MSG_TRUNC = 0x20
MSG_DONTWAIT = 0x40
MSG_WAITALL = 0x100
MSG_NOSIGNAL = 0x4000

# AI_*
AI_PASSIVE = 1
AI_CANONNAME = 2
AI_NUMERICHOST = 4
AI_NUMERICSERV = 0x400

# IPPROTO/IP-level
IP_HDRINCL = 3
IP_TOS = 1
IP_TTL = 2
IP_MULTICAST_TTL = 33
IP_MULTICAST_LOOP = 34
IP_ADD_MEMBERSHIP = 35
IP_DROP_MEMBERSHIP = 36

# TCP_*
TCP_NODELAY = 1
TCP_KEEPIDLE = 4
TCP_KEEPINTVL = 5
TCP_KEEPCNT = 6
TCP_USER_TIMEOUT = 18

# Address conversion endpoint sentinel
INADDR_ANY = 0
INADDR_LOOPBACK = 0x7F000001
INADDR_BROADCAST = 0xFFFFFFFF


SHUT_RD = 0
SHUT_WR = 1
SHUT_RDWR = 2


# `socket.SocketKind` namespace — CPython's socket exposes IntEnum-like
# .__members__ for tests.
class SocketKind:
    SOCK_STREAM = SOCK_STREAM
    SOCK_DGRAM = SOCK_DGRAM
    SOCK_RAW = SOCK_RAW


# Host info
def gethostname():
    return "localhost"


def gethostbyname(host):
    if host in ("localhost", "127.0.0.1"): return "127.0.0.1"
    raise error("gethostbyname: name resolution unsupported")


def gethostbyaddr(addr):
    raise error("gethostbyaddr: not supported")


def getservbyname(name, proto=None):
    raise error("getservbyname: not supported")


def getservbyport(port, proto=None):
    raise error("getservbyport: not supported")


def getfqdn(name=""):
    return name or gethostname()


def getaddrinfo(host, port, family=0, type=0, proto=0, flags=0):
    return [(AF_INET, SOCK_STREAM, IPPROTO_TCP, "", (host or "127.0.0.1", port or 0))]


def getnameinfo(addr, flags):
    return (addr[0], str(addr[1]))


def htonl(n): return n & 0xFFFFFFFF
def ntohl(n): return n & 0xFFFFFFFF
def htons(n): return n & 0xFFFF
def ntohs(n): return n & 0xFFFF


def inet_aton(s):
    parts = s.split(".")
    if len(parts) != 4: raise error("invalid IPv4 address")
    return bytes(int(p) for p in parts)


def inet_ntoa(b):
    if len(b) != 4: raise error("invalid IPv4 address")
    return ".".join(str(x) for x in b)


def inet_pton(family, addr):
    if family == AF_INET: return inet_aton(addr)
    raise error("inet_pton: only AF_INET supported")


def inet_ntop(family, packed):
    if family == AF_INET: return inet_ntoa(packed)
    raise error("inet_ntop: only AF_INET supported")


def has_ipv6(): return True


# socket.timeout / socket.gaierror / socket.herror — CPython exposes these.
class timeout(OSError):
    pass


class gaierror(error):
    pass


class herror(error):
    pass


class socket:
    """Stub socket — open() succeeds but I/O raises."""
    def __init__(self, family=AF_INET, type=SOCK_STREAM, proto=0, fileno=None):
        self.family = family
        self.type = type
        self.proto = proto
        self._closed = False
        self._timeout = None
    def fileno(self): return -1
    def setsockopt(self, *a, **kw): pass
    def getsockopt(self, level, opt, buflen=0):
        if buflen: return b"\x00" * buflen
        return 0
    def setblocking(self, b): pass
    def settimeout(self, t): self._timeout = t
    def gettimeout(self): return self._timeout
    def bind(self, addr): pass
    def listen(self, backlog=128): pass
    def accept(self): raise error("accept: stub socket cannot accept")
    def connect(self, addr): raise error("connect: stub socket cannot connect")
    def connect_ex(self, addr):
        try: self.connect(addr)
        except OSError as e: return e.errno or 1
        return 0
    def send(self, data, flags=0): return len(data)
    def sendall(self, data, flags=0): return None
    def sendto(self, data, *args): return len(data)
    def recv(self, n, flags=0): return b""
    def recvfrom(self, n, flags=0): return (b"", ("0.0.0.0", 0))
    def shutdown(self, how): pass
    def close(self):
        self._closed = True
    def detach(self): return -1
    def getsockname(self): return ("0.0.0.0", 0)
    def getpeername(self): raise error("getpeername: not connected")
    def __enter__(self): return self
    def __exit__(self, *exc):
        self.close()
        return False
    def makefile(self, mode="r", buffering=None, *, encoding=None, errors=None, newline=None):
        import io
        return io.StringIO("") if "b" not in mode else io.BytesIO(b"")


def socketpair(family=AF_UNIX, type=SOCK_STREAM, proto=0):
    return (socket(family, type, proto), socket(family, type, proto))


def create_connection(address, timeout=None, source_address=None, *, all_errors=False):
    s = socket()
    s.connect(address)
    return s


def create_server(address, *, family=AF_INET, backlog=None, reuse_port=False, dualstack_ipv6=False):
    s = socket(family, SOCK_STREAM)
    s.bind(address)
    return s


def fromfd(fd, family, type, proto=0):
    return socket(family, type, proto, fileno=fd)


def fromshare(info):
    raise error("fromshare: not supported")


SocketType = socket


def setdefaulttimeout(t): pass
def getdefaulttimeout(): return None


__all__ = ["socket", "socketpair", "create_connection", "create_server",
           "fromfd", "fromshare", "gethostname", "gethostbyname",
           "gethostbyaddr", "getservbyname", "getservbyport", "getfqdn",
           "getaddrinfo", "getnameinfo", "htonl", "ntohl", "htons", "ntohs",
           "inet_aton", "inet_ntoa", "inet_pton", "inet_ntop", "has_ipv6",
           "error", "timeout", "gaierror", "herror", "SocketType",
           "setdefaulttimeout", "getdefaulttimeout",
           "AF_UNSPEC", "AF_UNIX", "AF_INET", "AF_INET6",
           "SOCK_STREAM", "SOCK_DGRAM", "SOCK_RAW",
           "IPPROTO_IP", "IPPROTO_ICMP", "IPPROTO_TCP", "IPPROTO_UDP",
           "SOL_SOCKET", "SOL_TCP", "SOL_UDP",
           "SO_REUSEADDR", "SO_KEEPALIVE",
           "SHUT_RD", "SHUT_WR", "SHUT_RDWR",
           "INADDR_ANY", "INADDR_LOOPBACK", "INADDR_BROADCAST"]
