# Socket classes on top of the BSD-socket primitives in builtins/socket.c.
#
# A socket is an IO over the descriptor, so #read / #write / #gets / #close all
# come from IO for free; the socket-specific calls go straight to the __sock_*
# primitives.  Addresses are exchanged as [family, port, hostname, address] —
# the shape Addrinfo#to_a and BasicSocket#getsockname report — rather than as
# packed sockaddr blobs.

class SocketError < StandardError; end

class BasicSocket < IO
  # IO.new is a C singleton that wraps a descriptor, so socket subclasses cannot
  # inherit it (their ::new takes a host/port/path).  Route ::new back through
  # allocate + #initialize, and attach the fd with IO#__init_fd.
  def self.new(*args, &blk)
    s = allocate
    s.__send__(:initialize, *args, &blk)
    s
  end

  def self.for_fd(fd)
    s = allocate
    s.__send__(:__init_fd, fd, "r+")
    s
  end

  def do_not_reverse_lookup = @do_not_reverse_lookup.nil? ? true : @do_not_reverse_lookup
  def do_not_reverse_lookup=(v)
    @do_not_reverse_lookup = v
  end
  def self.do_not_reverse_lookup = @@dnrl ||= true
  def self.do_not_reverse_lookup=(v)
    @@dnrl = v
  end

  def getsockname_ary = __sock_name(fileno, false)
  def getpeername_ary = __sock_name(fileno, true)
  # The packed forms callers rarely inspect; keep the array shape available and
  # return a readable placeholder for the packed one.
  def getsockname = Socket.__pack(getsockname_ary)
  def getpeername = Socket.__pack(getpeername_ary)

  def local_address = Addrinfo.__from_ary(getsockname_ary)
  def remote_address = Addrinfo.__from_ary(getpeername_ary)

  def setsockopt(level, optname, value)
    __sock_setopt(fileno, Socket.__level(level), Socket.__optname(optname), value)
    0
  end

  def getsockopt(level, optname)
    Socket::Option.new(:INET, level, optname,
                       [__sock_getopt(fileno, Socket.__level(level), Socket.__optname(optname))].pack("i"))
  end

  def shutdown(how = Socket::SHUT_RDWR)
    how = { "SHUT_RD" => Socket::SHUT_RD, "SHUT_WR" => Socket::SHUT_WR,
            "SHUT_RDWR" => Socket::SHUT_RDWR, :SHUT_RD => Socket::SHUT_RD,
            :SHUT_WR => Socket::SHUT_WR, :SHUT_RDWR => Socket::SHUT_RDWR }.fetch(how, how)
    __sock_shutdown(fileno, how)
    0
  end

  def send(mesg, flags = 0, dest = nil) = __sock_send(fileno, mesg.to_s, flags)
  # Try the non-blocking op first and park only when it says EAGAIN: a POLL
  # wakeup is not a guarantee that the next call won't block.
  def recv(maxlen, flags = 0)
    loop do
      r = __sock_recv(fileno, maxlen, flags)
      return r if r
      wait_readable
    end
  end
  def recv_nonblock(maxlen, flags = 0, exception: true) = recv(maxlen, flags | Socket::MSG_DONTWAIT)
  def connect_address = local_address
end

class IPSocket < BasicSocket
  def addr(reverse_lookup = nil) = getsockname_ary
  def peeraddr(reverse_lookup = nil) = getpeername_ary
  def self.getaddress(host)
    r = __sock_getaddrinfo(host.to_s, nil, nil, Socket::SOCK_STREAM)
    raise SocketError, "getaddrinfo: no address for #{host}" if r.empty?
    r[0][3]
  end
end

class TCPSocket < IPSocket
  def initialize(host, port, local_host = nil, local_port = nil)
    fam = Socket.__family_of_host(host)      # "::1" must open an AF_INET6 socket
    fd = __sock_open(fam, Socket::SOCK_STREAM, 0)
    begin
      __sock_bind(fd, fam, local_host, local_port || 0) if local_host
      __sock_connect(fd, fam, host.to_s, port)   # Integer or a service name ("smtp")
    rescue Exception
      IO.new(fd).close rescue nil
      raise
    end
    __init_fd(fd, "r+")
  end

  def self.open(*args)
    s = new(*args)
    return s unless block_given?
    begin
      yield s
    ensure
      s.close unless s.closed?
    end
  end
end

class TCPServer < TCPSocket
  def initialize(host_or_port, port = nil)
    if port.nil?
      host, port = nil, host_or_port
    else
      host, port = host_or_port, port
    end
    fam = Socket.__family_of_host(host)
    fd = __sock_open(fam, Socket::SOCK_STREAM, 0)
    __sock_setopt(fd, Socket::SOL_SOCKET, Socket::SO_REUSEADDR, 1)
    __sock_bind(fd, fam, host, port)
    __sock_listen(fd, 5)
    __init_fd(fd, "r+")     # not TCPSocket#initialize: a server binds, never connects
  end

  def accept
    loop do
      pair = __sock_accept(fileno)
      if pair
        nfd, _addr = pair
        return TCPSocket.for_fd(nfd)
      end
      wait_readable
    end
  end

  def accept_nonblock(exception: true) = accept
  def listen(backlog) = (__sock_listen(fileno, backlog); 0)
  def self.open(*args)
    s = new(*args)
    return s unless block_given?
    begin
      yield s
    ensure
      s.close unless s.closed?
    end
  end
end

class UNIXSocket < BasicSocket
  def initialize(path)
    fd = __sock_open(Socket::AF_UNIX, Socket::SOCK_STREAM, 0)
    begin
      __sock_connect(fd, Socket::AF_UNIX, path.to_s, 0)
    rescue Exception
      IO.new(fd).close rescue nil
      raise
    end
    __init_fd(fd, "r+")
    @path = path.to_s
  end

  def path = @path
  def addr = ["AF_UNIX", @path.to_s]
  def peeraddr = ["AF_UNIX", getpeername_ary[2]]

  def self.pair(type = Socket::SOCK_STREAM, protocol = 0)
    a, b = __sock_pair(Socket::AF_UNIX, type)
    [for_fd(a), for_fd(b)]
  end
  class << self; alias_method :socketpair, :pair; end

  def self.open(path)
    s = new(path)
    return s unless block_given?
    begin
      yield s
    ensure
      s.close unless s.closed?
    end
  end
end

class UNIXServer < UNIXSocket
  def initialize(path)
    fd = __sock_open(Socket::AF_UNIX, Socket::SOCK_STREAM, 0)
    __sock_bind(fd, Socket::AF_UNIX, path.to_s, 0)
    __sock_listen(fd, 5)
    @path = path.to_s
    __init_fd(fd, "r+")
  end

  def accept
    loop do
      pair = __sock_accept(fileno)
      if pair
        nfd, _addr = pair
        return UNIXSocket.for_fd(nfd)
      end
      wait_readable
    end
  end

  def accept_nonblock(exception: true) = accept
  def listen(backlog) = (__sock_listen(fileno, backlog); 0)
  def self.open(path)
    s = new(path)
    return s unless block_given?
    begin
      yield s
    ensure
      s.close unless s.closed?
    end
  end
end

class UDPSocket < IPSocket
  def initialize(family = Socket::AF_INET)
    @family = Socket.__family(family)
    __init_fd(__sock_open(@family, Socket::SOCK_DGRAM, 0), "r+")
  end
  def family = @family

  def bind(host, port) = (__sock_bind(fileno, @family, host, port); 0)
  def connect(host, port) = (__sock_connect(fileno, @family, host, port); 0)
  def send(mesg, flags = 0, host = nil, port = nil)
    connect(host, port) if host
    __sock_send(fileno, mesg.to_s, flags)
  end
  def recvfrom(maxlen, flags = 0) = [recv(maxlen, flags), getpeername_ary]
end

class Socket < BasicSocket
  # Numeric constants come from the C side under __-prefixed globals so this file
  # stays free of platform ifdefs.
  AF_INET = __sock_const("AF_INET");    AF_INET6 = __sock_const("AF_INET6");   AF_UNIX = __sock_const("AF_UNIX")
  AF_UNSPEC = __sock_const("AF_UNSPEC")
  PF_INET = __sock_const("PF_INET");    PF_INET6 = __sock_const("PF_INET6");   PF_UNIX = __sock_const("PF_UNIX")
  SOCK_STREAM = __sock_const("SOCK_STREAM");  SOCK_DGRAM = __sock_const("SOCK_DGRAM")
  SOCK_RAW = __sock_const("SOCK_RAW");        SOCK_SEQPACKET = __sock_const("SOCK_SEQPACKET")
  SOL_SOCKET = __sock_const("SOL_SOCKET")
  IPPROTO_TCP = __sock_const("IPPROTO_TCP");  IPPROTO_UDP = __sock_const("IPPROTO_UDP");  IPPROTO_IP = __sock_const("IPPROTO_IP")
  SO_REUSEADDR = __sock_const("SO_REUSEADDR"); SO_KEEPALIVE = __sock_const("SO_KEEPALIVE")
  SO_BROADCAST = __sock_const("SO_BROADCAST"); SO_LINGER = __sock_const("SO_LINGER")
  SO_SNDBUF = __sock_const("SO_SNDBUF");       SO_RCVBUF = __sock_const("SO_RCVBUF")
  SO_TYPE = __sock_const("SO_TYPE");           SO_ERROR = __sock_const("SO_ERROR")
  TCP_NODELAY = __sock_const("TCP_NODELAY")
  SHUT_RD = __sock_const("SHUT_RD");  SHUT_WR = __sock_const("SHUT_WR");  SHUT_RDWR = __sock_const("SHUT_RDWR")
  MSG_PEEK = __sock_const("MSG_PEEK"); MSG_OOB = __sock_const("MSG_OOB")
  MSG_DONTROUTE = __sock_const("MSG_DONTROUTE"); MSG_WAITALL = __sock_const("MSG_WAITALL")
  MSG_DONTWAIT = 0x40   # not exposed by the C table; POSIX value
  AI_PASSIVE = __sock_const("AI_PASSIVE"); AI_CANONNAME = __sock_const("AI_CANONNAME"); AI_NUMERICHOST = __sock_const("AI_NUMERICHOST")
  NI_NUMERICHOST = __sock_const("NI_NUMERICHOST"); NI_NUMERICSERV = __sock_const("NI_NUMERICSERV")
  INADDR_ANY = __sock_const("INADDR_ANY"); INADDR_LOOPBACK = __sock_const("INADDR_LOOPBACK")

  # The rest of the platform's table.  __sock_const returns nil for a name this
  # build does not have, so the list can be generous.
  %w[
    PF_UNSPEC AF_PACKET PF_PACKET SOCK_RDM SOCK_PACKET
    IPPROTO_IPV6 IPPROTO_ICMP IPPROTO_RAW IPPROTO_HOPOPTS
    IP_TTL IP_RECVTTL IP_PKTINFO IP_MTU IP_MULTICAST_TTL IP_MULTICAST_LOOP
    IPV6_PKTINFO IPV6_NEXTHOP IPV6_V6ONLY
    TCP_CORK TCP_INFO TCP_KEEPIDLE TCP_KEEPINTVL TCP_KEEPCNT UDP_CORK
    SCM_RIGHTS SCM_CREDENTIALS SCM_TIMESTAMP
    SO_REUSEPORT SO_DONTROUTE SO_OOBINLINE SO_RCVLOWAT SO_SNDLOWAT
    SO_RCVTIMEO SO_SNDTIMEO SO_ACCEPTCONN SO_PEERCRED
    MSG_TRUNC MSG_CTRUNC MSG_MORE MSG_NOSIGNAL
    AI_ADDRCONFIG AI_ALL AI_V4MAPPED
    NI_NAMEREQD NI_NOFQDN NI_DGRAM
    EAI_NONAME EAI_AGAIN EAI_FAIL EAI_FAMILY EAI_SERVICE EAI_SOCKTYPE
  ].each { |nm| (v = __sock_const(nm)) && const_set(nm, v) }

  # Raised by name-resolution failures (Ruby 3.3+); a SocketError so older
  # rescues keep working.
  ResolutionError = Class.new(SocketError)

  module Constants; end
  constants.each { |cn| Constants.const_set(cn, const_get(cn)) if const_get(cn).is_a?(Integer) }

  class Option
    attr_reader :family, :level, :optname, :data
    def initialize(family, level, optname, data)
      @family, @level, @optname, @data = family, level, optname, data
    end
    def int = @data.unpack("i")[0]
    def bool = int != 0
    def to_s = @data
    def unpack(fmt) = @data.unpack(fmt)
    def self.int(family, level, optname, integer) = new(family, level, optname, [integer].pack("i"))
    def self.bool(family, level, optname, bool) = int(family, level, optname, bool ? 1 : 0)
  end

  class AncillaryData
    attr_reader :family, :level, :type, :data
    def initialize(family, level, type, data)
      @family, @level, @type, @data = family, level, type, data
    end
    def int = @data.unpack("i")[0]
  end

  def initialize(family, type, protocol = 0)
    @family, @type = Socket.__family(family), Socket.__socktype(type)
    __init_fd(__sock_open(@family, @type, protocol), "r+")
  end

  def bind(addr) = (a = Socket.__unpack(addr); __sock_bind(fileno, @family, a[2], a[1]); 0)
  def connect(addr) = (a = Socket.__unpack(addr); __sock_connect(fileno, @family, a[2], a[1]); 0)
  def listen(backlog) = (__sock_listen(fileno, backlog); 0)

  def accept
    loop do
      pair = __sock_accept(fileno)
      if pair
        nfd, addr = pair
        return [Socket.for_fd(nfd), Addrinfo.__from_ary(addr)]
      end
      wait_readable
    end
  end

  def self.pair(family, type, protocol = 0)
    a, b = __sock_pair(__family(family), __socktype(type))
    [for_fd(a), for_fd(b)]
  end
  class << self; alias_method :socketpair, :pair; end

  def self.gethostname = __sock_hostname

  # getnameinfo(sockaddr, flags = 0) → [hostname, service].  The sockaddr may be
  # packed bytes, an Addrinfo, or the descriptive [family, port, host, addr].
  def self.getnameinfo(sa, flags = 0)
    packed = sa.is_a?(String) ? sa : __pack(__unpack(sa))
    __sock_getnameinfo(packed, flags)
  end

  def self.getaddrinfo(host, service, family = nil, socktype = nil, protocol = nil, flags = nil)
    __sock_getaddrinfo(host&.to_s, service, family && __family(family), socktype)
  end

  # nil family = infer from the host string, so sockaddr_in accepts IPv6 too.
  def self.sockaddr_in(port, host) = __sock_pack(nil, Integer(port), host.to_s)
  class << self; alias_method :pack_sockaddr_in, :sockaddr_in; end
  def self.sockaddr_un(path) = __sock_pack(AF_UNIX, 0, path.to_s)
  class << self; alias_method :pack_sockaddr_un, :sockaddr_un; end

  def self.unpack_sockaddr_in(sa)
    a = __unpack(sa)
    raise ArgumentError, "not an AF_INET/AF_INET6 sockaddr" if a[0] == "AF_UNIX"
    [a[1], a[3]]
  end

  def self.unpack_sockaddr_un(sa)
    a = __unpack(sa)
    raise ArgumentError, "not an AF_UNIX sockaddr" unless a[0] == "AF_UNIX"
    a[2]
  end

  # A packed address is the real struct sockaddr bytes (binary String); the
  # descriptive [family, port, host, addr] Array is what the rest of this file
  # passes around, so __pack/__unpack convert between the two.  An Array given
  # where a packed address is expected is accepted as already-unpacked.
  def self.__pack(ary) = __sock_pack(ary[0], ary[1] || 0, (ary[3] || ary[2]).to_s)
  def self.__unpack(sa)
    return sa if sa.is_a?(Array)
    return sa.to_a if sa.is_a?(Addrinfo)
    __sock_unpack(sa.to_str)
  end

  # The address family a host string needs: "::1" is AF_INET6, "127.0.0.1" is
  # AF_INET, a name is whatever the resolver says.  nil/"" means "any" → AF_INET.
  def self.__family_of_host(host)
    h = host.to_s
    return AF_INET if h.empty?
    r = (__sock_getaddrinfo(h, nil, nil, SOCK_STREAM) rescue nil)
    (r && r[0]) ? __family(r[0][0]) : AF_INET
  end

  def self.__socktype(t)
    return t if t.is_a?(Integer)
    n = t.to_s.upcase.sub(/\ASOCK_/, "")
    { "STREAM" => SOCK_STREAM, "DGRAM" => SOCK_DGRAM,
      "RAW" => SOCK_RAW, "SEQPACKET" => SOCK_SEQPACKET }.fetch(n, SOCK_STREAM)
  end

  def self.__family(f)
    return f if f.is_a?(Integer)
    n = f.to_s.sub(/\AAF_|\APF_/, "")
    { "INET" => AF_INET, "INET6" => AF_INET6, "UNIX" => AF_UNIX,
      "LOCAL" => AF_UNIX, "UNSPEC" => AF_UNSPEC }.fetch(n.upcase, AF_INET)
  end

  def self.__level(l)
    return l if l.is_a?(Integer)
    n = l.to_s.upcase.sub(/\ASOL_/, "")
    { "SOCKET" => SOL_SOCKET, "TCP" => IPPROTO_TCP, "IP" => IPPROTO_IP, "UDP" => IPPROTO_UDP }.fetch(n, SOL_SOCKET)
  end

  def self.__optname(o)
    return o if o.is_a?(Integer)
    n = o.to_s.upcase.sub(/\ASO_/, "")
    { "REUSEADDR" => SO_REUSEADDR, "KEEPALIVE" => SO_KEEPALIVE, "BROADCAST" => SO_BROADCAST,
      "LINGER" => SO_LINGER, "SNDBUF" => SO_SNDBUF, "RCVBUF" => SO_RCVBUF,
      "TYPE" => SO_TYPE, "ERROR" => SO_ERROR, "NODELAY" => TCP_NODELAY }.fetch(n, SO_REUSEADDR)
  end
end

# Addrinfo — one resolved endpoint.
class Addrinfo
  attr_reader :afamily, :pfamily, :socktype, :protocol

  def self.__from_ary(a)
    ai = allocate
    ai.__setup(a[0], a[1], a[2], a[3], a[4] || Socket::SOCK_STREAM, a[5] || 0)
    ai
  end

  def __setup(fam, port, host, addr, socktype, protocol)
    @famname = fam
    @port = port
    @host = host
    @addr = addr
    @socktype = socktype
    @protocol = protocol
    @afamily = Socket.__family(fam)
    @pfamily = @afamily
    self
  end

  def initialize(sockaddr, family = nil, socktype = nil, protocol = nil)
    a = sockaddr.is_a?(Array) ? sockaddr : Socket.__unpack(sockaddr)
    __setup(a[0], a[1] || 0, a[2], a[3] || a[2], socktype || Socket::SOCK_STREAM, protocol || 0)
  end

  def self.getaddrinfo(host, service, family = nil, socktype = nil, protocol = nil, flags = nil)
    Socket.getaddrinfo(host, service, family, socktype, protocol, flags).map { |a| __from_ary(a) }
  end

  # Addrinfo.ip leaves socktype/protocol unspecified (0), unlike .tcp/.udp.
  def self.ip(host)
    r = getaddrinfo(host, nil, nil, Socket::SOCK_STREAM)
    raise SocketError, "getaddrinfo: no address for #{host}" if r.empty?
    a = r[0].to_a
    __from_ary([a[0], 0, a[2], a[3], 0, 0])
  end

  def self.tcp(host, port) = getaddrinfo(host, port, nil, Socket::SOCK_STREAM).first
  def self.udp(host, port) = getaddrinfo(host, port, nil, Socket::SOCK_DGRAM).first

  def self.unix(path, socktype = Socket::SOCK_STREAM)
    __from_ary(["AF_UNIX", 0, path.to_s, path.to_s, socktype, 0])
  end

  def ip? = @famname == "AF_INET" || @famname == "AF_INET6"
  def ipv4? = @famname == "AF_INET"
  def ipv6? = @famname == "AF_INET6"
  def unix? = @famname == "AF_UNIX"
  def ip_address = (raise SocketError, "need IPv4 or IPv6 address" unless ip?; @addr.to_s)
  def ip_port = (raise SocketError, "need IPv4 or IPv6 address" unless ip?; @port.to_i)
  def unix_path = (raise SocketError, "need AF_UNIX address" unless unix?; @host.to_s)
  def to_a = [@famname, @port, @host, @addr]
  def to_s = @addr.to_s

  def inspect_sockaddr
    return @host.to_s unless ip?
    a = ipv6? ? "[#{@addr}]" : @addr.to_s
    @port.to_i == 0 ? a : "#{a}:#{@port}"
  end

  # "#<Addrinfo: 127.0.0.1:80 TCP>" — the trailing word names the socktype,
  # spelled TCP/UDP for the two combinations that have a common name.
  def inspect
    tail =
      if ip? && @socktype == Socket::SOCK_STREAM && @protocol == Socket::IPPROTO_TCP then " TCP"
      elsif ip? && @socktype == Socket::SOCK_DGRAM && @protocol == Socket::IPPROTO_UDP then " UDP"
      elsif @socktype == Socket::SOCK_STREAM then " SOCK_STREAM"
      elsif @socktype == Socket::SOCK_DGRAM  then " SOCK_DGRAM"
      elsif @socktype == Socket::SOCK_RAW    then " SOCK_RAW"
      else ""
      end
    "#<Addrinfo: #{inspect_sockaddr}#{tail}>"
  end
  def to_sockaddr = Socket.__pack(to_a)
  alias_method :to_str, :to_sockaddr
  def canonname = nil
  def ==(other) = other.is_a?(Addrinfo) && to_a == other.to_a
  alias_method :eql?, :==
  def hash = to_a.hash

  def connect
    s = ip? ? TCPSocket.new(@addr, @port) : UNIXSocket.new(@host)
    return s unless block_given?
    begin
      yield s
    ensure
      s.close unless s.closed?
    end
  end

  def bind
    s = ip? ? TCPServer.new(@addr, @port) : UNIXServer.new(@host)
    return s unless block_given?
    begin
      yield s
    ensure
      s.close unless s.closed?
    end
  end

  def listen(backlog = 5)
    s = bind
    s.listen(backlog)
    s
  end

  def family_addrinfo(*args) = self
  def afamily_name = @famname
end
