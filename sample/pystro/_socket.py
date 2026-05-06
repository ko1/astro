"""pystro stub for `_socket` (the C accelerator for `socket`)."""

# Address families.
AF_UNSPEC = 0
AF_INET = 2
AF_INET6 = 10
AF_UNIX = 1

# Socket types.
SOCK_STREAM = 1
SOCK_DGRAM = 2
SOCK_RAW = 3
SOCK_SEQPACKET = 5
SOCK_RDM = 4

# Protocols.
IPPROTO_IP = 0
IPPROTO_ICMP = 1
IPPROTO_TCP = 6
IPPROTO_UDP = 17
IPPROTO_IPV6 = 41
IPPROTO_RAW = 255

# Options.
SOL_SOCKET = 1
SO_REUSEADDR = 2
SO_BROADCAST = 6
SO_KEEPALIVE = 9
SO_LINGER = 13
SO_RCVBUF = 8
SO_SNDBUF = 7
SO_REUSEPORT = 15
SO_ERROR = 4
SO_TYPE = 3

TCP_NODELAY = 1

# Constants commonly tested.
INADDR_ANY = 0
INADDR_LOOPBACK = 0x7f000001
INADDR_BROADCAST = 0xffffffff

# Flags / shutdown.
SHUT_RD = 0
SHUT_WR = 1
SHUT_RDWR = 2

MSG_DONTWAIT = 64
MSG_PEEK = 2
MSG_WAITALL = 256

AI_PASSIVE = 1
AI_CANONNAME = 2
AI_NUMERICHOST = 4

NI_NUMERICHOST = 1
NI_NUMERICSERV = 2

EAI_AGAIN = -3
EAI_FAIL = -4
EAI_NONAME = -2

has_ipv6 = True


class error(OSError):
    pass


class herror(error):
    pass


class gaierror(error):
    pass


class timeout(error):
    pass


class socket:
    """Pystro placeholder — networking isn't implemented."""
    def __init__(self, family=AF_INET, type=SOCK_STREAM, proto=0, fileno=None):
        self.family = family
        self.type = type
        self.proto = proto
        self._fileno = fileno
    def close(self): pass
    def fileno(self): return self._fileno or -1
    def setsockopt(self, *a, **k): pass
    def getsockopt(self, *a, **k): return 0
    def bind(self, addr): raise error("socket not supported")
    def listen(self, *a): raise error("socket not supported")
    def accept(self): raise error("socket not supported")
    def connect(self, addr): raise error("socket not supported")
    def connect_ex(self, addr): return 1
    def send(self, *a, **k): raise error("socket not supported")
    def recv(self, *a, **k): raise error("socket not supported")
    def sendto(self, *a, **k): raise error("socket not supported")
    def recvfrom(self, *a, **k): raise error("socket not supported")
    def settimeout(self, t): pass
    def gettimeout(self): return None
    def setblocking(self, b): pass
    def getblocking(self): return True
    def shutdown(self, h): pass


def socketpair(family=AF_UNIX, type=SOCK_STREAM, proto=0):
    raise error("socketpair not supported")


def gethostname():
    return "localhost"


def gethostbyname(name):
    return "127.0.0.1"


def gethostbyaddr(addr):
    return ("localhost", [], [addr])


def getaddrinfo(host, port, *args, **kwargs):
    return []


def getnameinfo(addr, flags):
    return ("localhost", "0")


def getservbyname(name, proto=None):
    return 0


def getservbyport(port, proto=None):
    return ""


def getdefaulttimeout():
    return None


def setdefaulttimeout(t):
    pass


def htons(x): return x
def htonl(x): return x
def ntohs(x): return x
def ntohl(x): return x


def inet_aton(s): return b"\x00\x00\x00\x00"
def inet_ntoa(b): return "0.0.0.0"
def inet_pton(family, s): return b"\x00\x00\x00\x00"
def inet_ntop(family, b): return "0.0.0.0"


SocketType = socket
__all__ = ["error", "herror", "gaierror", "timeout", "socket",
           "SocketType", "AF_INET", "AF_INET6", "AF_UNIX", "AF_UNSPEC",
           "SOCK_STREAM", "SOCK_DGRAM", "SOCK_RAW", "SOL_SOCKET",
           "SO_REUSEADDR", "SO_BROADCAST", "TCP_NODELAY",
           "gethostname", "gethostbyname", "gethostbyaddr",
           "getaddrinfo", "getnameinfo",
           "htons", "htonl", "ntohs", "ntohl",
           "inet_aton", "inet_ntoa", "inet_pton", "inet_ntop",
           "has_ipv6", "INADDR_ANY"]
