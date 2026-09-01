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
    # IO.new's rb_warn, inherited by every socket class: only ::open takes a block
    warn "warning: #{self}::new() does not take block; use #{self}::open() instead" if blk && !$VERBOSE.nil?
    s = allocate
    s.__send__(:initialize, *args, &blk)
    s
  end

  def self.for_fd(fd)
    s = allocate
    s.__send__(:__init_fd, fd, "r+b")   # every socket is binary in CRuby
    s
  end

  # CRuby snapshots the class-wide setting when the socket is created, so a later
  # change to BasicSocket.do_not_reverse_lookup does not reach existing sockets.
  def do_not_reverse_lookup
    @do_not_reverse_lookup = BasicSocket.do_not_reverse_lookup if @do_not_reverse_lookup.nil?
    @do_not_reverse_lookup
  end
  def do_not_reverse_lookup=(v)
    @do_not_reverse_lookup = v ? true : false
  end
  # `||=` would make a stored `false` read back as true
  def self.do_not_reverse_lookup = defined?(@@dnrl) ? @@dnrl : true
  def self.do_not_reverse_lookup=(v)
    @@dnrl = v ? true : false
  end

  def getsockname_ary = __sock_name(fileno, false)
  def getpeername_ary = __sock_name(fileno, true)
  # The packed forms callers rarely inspect; keep the array shape available and
  # return a readable placeholder for the packed one.
  def getsockname = Socket.__pack(getsockname_ary)
  def getpeername = Socket.__pack(getpeername_ary)

  # The Addrinfo must describe THIS socket, so take the type/protocol from it
  # rather than defaulting to SOCK_STREAM.
  private def __own_addrinfo(ary)
    st = getsockopt(:SOCKET, :TYPE).int rescue Socket::SOCK_STREAM
    Addrinfo.__from_ary([ary[0], ary[1], ary[2], ary[3], st, 0])
  end
  def local_address = __own_addrinfo(getsockname_ary)
  def remote_address = __own_addrinfo(getpeername_ary)

  # setsockopt(level, optname, value) or setsockopt(Socket::Option).
  def setsockopt(level, optname = nil, value = nil)
    if level.is_a?(Socket::Option) && optname.nil?
      opt = level
      __sock_setopt(fileno, opt.level, opt.optname, opt.data)
      return 0
    end
    raise ArgumentError, "wrong number of arguments (given 1, expected 3)" if optname.nil?
    lv = Socket.__level_strict(level)
    # nil is not a value: CRuby will not guess (true/false and Integer are).
    raise TypeError, "no implicit conversion of nil into Integer" if value.nil?
    __sock_setopt(fileno, lv, Socket.__optname(optname, lv), value)
    0
  end

  def getsockopt(level, optname)
    lv = Socket.__level_strict(level)
    nm = Socket.__optname(optname, lv)
    Socket::Option.new(:INET, lv, nm, __sock_getopt_raw(fileno, lv, nm))
  end

  def shutdown(how = Socket::SHUT_RDWR)
    how = how.to_int if !how.is_a?(Integer) && !how.is_a?(Symbol) && !how.is_a?(String) && how.respond_to?(:to_int)
    how = how.to_str if !how.is_a?(Integer) && !how.is_a?(Symbol) && !how.is_a?(String) && how.respond_to?(:to_str)
    unless how.is_a?(Integer)
      n = how.to_s.upcase.sub(/\ASHUT_/, "")
      how = { "RD" => Socket::SHUT_RD, "WR" => Socket::SHUT_WR,
              "RDWR" => Socket::SHUT_RDWR }.fetch(n) do
        raise SocketError, "unknown shutdown argument: #{n}"
      end
    end
    unless [Socket::SHUT_RD, Socket::SHUT_WR, Socket::SHUT_RDWR].include?(how)
      raise ArgumentError, "invalid shutdown mode: #{how}"
    end
    __sock_shutdown(fileno, how)
    0
  end

  # `dest` is a packed sockaddr (or an Addrinfo); without one the socket must
  # already be connected.  It was being ignored, so a sendto silently became a
  # send on an unconnected socket.
  def send(mesg, flags = 0, dest = nil)
    return __sock_send(fileno, mesg.to_s, flags) if dest.nil?
    a = Socket.__unpack(dest.is_a?(Addrinfo) ? dest.to_sockaddr : dest)
    __sock_sendto(fileno, mesg.to_s, flags, a[0], a[2], a[1])
  end
  # Try the non-blocking op first and park only when it says EAGAIN: a POLL
  # wakeup is not a guarantee that the next call won't block.
  def recv(maxlen, flags = 0, outbuf = nil)
    loop do
      r = __sock_recv(fileno, maxlen, flags)
      if r
        return r unless outbuf
        enc = outbuf.encoding
        outbuf.replace(r)
        outbuf.force_encoding(enc)
        return outbuf
      end
      wait_readable
    end
  end
  def recv_nonblock(maxlen, flags = 0, exception: true) = recv(maxlen, flags | Socket::MSG_DONTWAIT)
  def connect_address = local_address

  # koruby always closes the descriptor with the IO, so autoclose is a recorded
  # preference rather than a behaviour switch.
  def autoclose? = @autoclose.nil? ? true : @autoclose
  def autoclose=(v); @autoclose = v ? true : false; v; end

  # recvfrom → [data, sender_address].  The blocking form parks and retries;
  # "poll said readable" is not a promise that the next call won't block.
  # An optional third argument is an output buffer: it is filled in place and
  # returned, keeping its own encoding (CRuby).
  def recvfrom(maxlen, flags = 0, outbuf = nil)
    loop do
      r = __sock_recvfrom(fileno, maxlen, flags)
      if r
        next_data = r[0]
        if outbuf
          enc = outbuf.encoding
          outbuf.replace(next_data)
          outbuf.force_encoding(enc)
          next_data = outbuf
        end
        return [next_data, r[1]]
      end
      wait_readable
    end
  end

  def recvfrom_nonblock(maxlen = 65536, flags = 0, outbuf = nil, exception: true)
    r = __sock_recvfrom(fileno, maxlen, flags)
    unless r
      return :wait_readable unless exception
      raise IO::EAGAINWaitReadable, "recvfrom(2) would block"
    end
    outbuf.replace(r[0]) if outbuf
    [outbuf || r[0], r[1]]
  end

  # recvmsg → [data, Addrinfo, flags, *controls].  koruby has no ancillary-data
  # plumbing, so the control list is always empty; everything else is faithful.
  # Socket#recvfrom answers an Addrinfo where BasicSocket's answers the raw
  # tuple, so accept either shape here.
  private def __as_addrinfo(a) = a.is_a?(Addrinfo) ? a : Addrinfo.__from_ary(a)

  def recvmsg(maxlen = nil, flags = 0, opts = nil, scm_rights: false)
    data, addr = recvfrom(maxlen || 65536, flags)
    [data, __as_addrinfo(addr), 0]
  end

  def recvmsg_nonblock(maxlen = nil, flags = 0, opts = nil, scm_rights: false, exception: true)
    r = recvfrom_nonblock(maxlen || 65536, flags, nil, exception: exception)
    return r unless r.is_a?(Array)
    [r[0], __as_addrinfo(r[1]), 0]
  end

  def sendmsg(mesg, flags = 0, dest_sockaddr = nil, *controls)
    if dest_sockaddr
      a = Socket.__unpack(dest_sockaddr)
      __sock_sendto(fileno, mesg.to_s, flags, a[0], a[2], a[1])
    else
      __sock_send(fileno, mesg.to_s, flags)
    end
  end
  def sendmsg_nonblock(mesg, flags = 0, dest_sockaddr = nil, *controls, exception: true)
    sendmsg(mesg, flags | Socket::MSG_DONTWAIT, dest_sockaddr, *controls)
  rescue Errno::EAGAIN, Errno::EWOULDBLOCK
    # CRuby surfaces "would block" as a WaitWritable, or as a Symbol on request
    return :wait_writable unless exception
    raise IO::EAGAINWaitWritable, "sendmsg(2) would block"
  end
end

class IPSocket < BasicSocket
  # true / :hostname ask for the PTR name in the host slot; false / :numeric (and
  # the default) leave the numeric address there.
  private def __rev(ary, reverse_lookup)
    want = case reverse_lookup
           when true, :hostname then true
           when false, :numeric then false
           when nil then !do_not_reverse_lookup
           else raise ArgumentError, "invalid reverse_lookup flag: :#{reverse_lookup}"
           end
    return ary unless want
    ary = ary.dup
    ary[2] = begin
      Socket.getnameinfo([ary[0], ary[1], ary[3], ary[3]])[0]
    rescue StandardError
      ary[3]
    end
    ary
  end

  def addr(reverse_lookup = nil) = __rev(getsockname_ary, reverse_lookup)
  def peeraddr(reverse_lookup = nil) = __rev(getpeername_ary, reverse_lookup)

  # #recvfrom follows the socket's own reverse-lookup setting.
  def recvfrom(maxlen, flags = 0, outbuf = nil)
    mesg, addr = super
    [mesg, __rev(addr, nil)]
  end
  def self.getaddress(host)
    r = __sock_getaddrinfo(host.to_s, nil, nil, Socket::SOCK_STREAM)
    raise SocketError, "getaddrinfo: no address for #{host}" if r.empty?
    r[0][3]
  end
end

class TCPSocket < IPSocket
  # :connect_timeout / :open_timeout bound the connect; :resolv_timeout is
  # accepted and ignored (resolution here is a single getaddrinfo call).
  def initialize(host, port, local_host = nil, local_port = nil,
                 connect_timeout: nil, open_timeout: nil, resolv_timeout: nil)
    tmo = connect_timeout || open_timeout
    fam = Socket.__family_of_host(host)      # "::1" must open an AF_INET6 socket
    fd = __sock_open(fam, Socket::SOCK_STREAM, 0)
    begin
      __sock_bind(fd, fam, local_host, local_port || 0) if local_host
      __sock_connect(fd, fam, host.to_s, port, tmo)   # Integer or a service name ("smtp")
    rescue Exception
      IO.new(fd).close rescue nil
      raise
    end
    __init_fd(fd, "r+b")
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

  # Like Socket.gethostbyname but with the addresses as numeric strings.
  def self.gethostbyname(name)
    h = __sock_hostent(name.to_s)
    [h[0], h[1], h[2], *h[4]]
  end
end

class TCPServer < TCPSocket
  # TCPServer.new(port) or TCPServer.new(host, port); the arity decides, so an
  # explicit nil port is "any port on this host", not "this is the port".
  def initialize(*args)
    unless (1..2).cover?(args.size)
      raise ArgumentError, "wrong number of arguments (given #{args.size}, expected 1..2)"
    end
    host, port = args.size == 1 ? [nil, args[0]] : args
    # nil / "" mean "any port"; anything else must be an Integer or a name
    if port.nil? || port == ""
      port = 0
    elsif !port.is_a?(Integer) && !port.is_a?(String)
      unless port.respond_to?(:to_str) || port.respond_to?(:to_int)
        raise TypeError, "no implicit conversion of #{port.class} into String"
      end
      port = port.respond_to?(:to_int) ? port.to_int : port.to_str
      port = 0 if port == ""
    end
    fam = Socket.__family_of_host(host)
    fd = __sock_open(fam, Socket::SOCK_STREAM, 0)
    __sock_setopt(fd, Socket::SOL_SOCKET, Socket::SO_REUSEADDR, 1)
    __sock_bind(fd, fam, host, port)
    __sock_listen(fd, 5)
    __init_fd(fd, "r+b")     # not TCPSocket#initialize: a server binds, never connects
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

  # Like #accept, but hands back the raw descriptor.
  def sysaccept
    loop do
      pair = __sock_accept(fileno)
      return pair[0] if pair
      wait_readable
    end
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

class UNIXSocket < BasicSocket
  # Descriptor passing over SCM_RIGHTS.  #recv_io answers `klass.for_fd`, or the
  # raw Integer when klass is nil.
  def send_io(io)
    __sock_send_fd(fileno, io.respond_to?(:fileno) ? io.fileno : io.to_int)
    nil
  end

  def recv_io(klass = IO, mode = nil)
    fd = __sock_recv_fd(fileno)
    return fd if klass.nil? || fd.nil?
    mode.nil? ? klass.for_fd(fd) : klass.for_fd(fd, mode)
  end

  # An embedded NUL would truncate the sun_path handed to connect(2)/bind(2)
  # (CVE-2018-8779), so reject it the way CRuby's rb_get_path does.
  def self.__check_path(path)
    s = path.to_s
    raise ArgumentError, "path name contains null byte" if s.include?("\0")
    s
  end

  def initialize(path)
    path = UNIXSocket.__check_path(path)
    fd = __sock_open(Socket::AF_UNIX, Socket::SOCK_STREAM, 0)
    begin
      __sock_connect(fd, Socket::AF_UNIX, path.to_s, 0)
    rescue Exception
      IO.new(fd).close rescue nil
      raise
    end
    __init_fd(fd, "r+b")
    @path = path.to_s
  end

  def path = @path
  def addr = ["AF_UNIX", @path.to_s]
  def peeraddr = ["AF_UNIX", getpeername_ary[2]]

  # A UNIX socket's sender address is just ["AF_UNIX", path] — an unbound peer
  # (a socketpair, or a client that never bound) reports an empty path.
  def recvfrom(maxlen, flags = 0, outbuf = nil)
    mesg, _ = super
    [mesg, ["AF_UNIX", (getpeername_ary[2].to_s rescue "")]]
  end

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
    path = UNIXSocket.__check_path(path)
    fd = __sock_open(Socket::AF_UNIX, Socket::SOCK_STREAM, 0)
    __sock_bind(fd, Socket::AF_UNIX, path.to_s, 0)
    __sock_listen(fd, 5)
    @path = path.to_s
    __init_fd(fd, "r+b")
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

  # Like #accept, but hands back the raw descriptor.
  def sysaccept
    loop do
      pair = __sock_accept(fileno)
      return pair[0] if pair
      wait_readable
    end
  end

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
    __init_fd(__sock_open(@family, Socket::SOCK_DGRAM, 0), "r+b")
  end
  def family = @family

  def bind(host, port) = (__sock_bind(fileno, @family, host, port); 0)
  def connect(host, port) = (__sock_connect(fileno, @family, host, port); 0)
  # UDPSocket#send takes either (host, port) or a single packed sockaddr.
  def send(mesg, flags = 0, host = nil, port = nil)
    return __sock_send(fileno, mesg.to_s, flags) if host.nil?
    if port.nil?
      a = Socket.__unpack(host.is_a?(Addrinfo) ? host.to_sockaddr : host)
      return __sock_sendto(fileno, mesg.to_s, flags, a[0], a[2], a[1])
    end
    __sock_sendto(fileno, mesg.to_s, flags, @family, host.to_s, port)
  end
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
  class ResolutionError < SocketError
    # The EAI_* code getaddrinfo(3) returned.
    def error_code = @error_code
  end

  # Everything else the platform defines, straight from the C table — the names
  # above stay explicit because this file refers to them while loading.
  __sock_const_list.each { |n, v| const_set(n, v) unless const_defined?(n, false) }

  module Constants; end
  constants.each { |cn| Constants.const_set(cn, const_get(cn)) if const_get(cn).is_a?(Integer) }

  class Option
    attr_reader :family, :level, :optname, :data
    def initialize(family, level, optname, data)
      @family  = Socket.__family_strict(family)
      @level   = Socket.__level_strict(level)
      @optname = Socket.__optname(optname, @level)
      @data    = data.to_str
    end
    def int = @data.unpack("i")[0]
    def bool = int != 0
    def to_s = @data
    def unpack(fmt) = @data.unpack(fmt)
    def self.int(family, level, optname, integer) = new(family, level, optname, [integer].pack("i"))
    def self.bool(family, level, optname, bool) = int(family, level, optname, bool ? 1 : 0)

    # SO_LINGER carries a `struct linger` (two ints), not a single one.
    def self.linger(onoff, secs)
      on = onoff.is_a?(Integer) ? onoff : (onoff ? 1 : 0)
      new(Socket::AF_UNSPEC, Socket::SOL_SOCKET, Socket::SO_LINGER, [on, secs.to_int].pack("ii"))
    end

    def linger
      unless @level == Socket::SOL_SOCKET && @optname == Socket::SO_LINGER
        raise TypeError, "linger socket option expected"
      end
      # struct linger is two ints; anything shorter is not one
      raise TypeError, "size differ. expected as sizeof(struct linger)=8 but #{@data.bytesize}" if @data.bytesize < 8
      on, secs = @data.unpack("ii")
      [on != 0, secs]
    end
  end

  class AncillaryData
    attr_reader :family, :level, :type, :data

    # CRuby stores the NUMERIC family/level/type; symbols and strings
    # ("INET", :IPPROTO_IP, :RECVTTL) are resolved through the constants.
    def initialize(family, level, type, data)
      family = family.to_str if !family.is_a?(Integer) && !family.is_a?(Symbol) && !family.is_a?(String) && family.respond_to?(:to_str)
      @family = Socket.__family(family)
      @level  = AncillaryData.__level(@family, level)
      @type   = AncillaryData.__type(@family, @level, type)
      @data   = data.to_str
    end

    def self.__level(fam, lv)
      lv = lv.to_str if !lv.is_a?(Integer) && !lv.is_a?(Symbol) && !lv.is_a?(String) && lv.respond_to?(:to_str)
      return lv if lv.is_a?(Integer)
      n = lv.to_s.upcase
      n = "SOL_SOCKET" if n == "SOCKET"
      n = "IPPROTO_#{n}" unless n.start_with?("SOL_") || n.start_with?("IPPROTO_")
      Socket.const_defined?(n) ? Socket.const_get(n) : (raise SocketError, "unknown protocol level: #{lv}")
    end

    def self.__type(fam, level, ty)
      ty = ty.to_str if !ty.is_a?(Integer) && !ty.is_a?(Symbol) && !ty.is_a?(String) && ty.respond_to?(:to_str)
      return ty if ty.is_a?(Integer)
      n = ty.to_s.upcase
      # only this level's own constant family counts: :RECVTTL is an IP option,
      # so it must not resolve under SOL_SOCKET
      cands = case level
              when Socket::SOL_SOCKET  then [n, "SCM_#{n}"]
              when Socket::IPPROTO_IP  then [n, "IP_#{n}"]
              when Socket::IPPROTO_TCP then [n, "TCP_#{n}"]
              when (Socket.const_defined?(:IPPROTO_IPV6) ? Socket::IPPROTO_IPV6 : nil) then [n, "IPV6_#{n}"]
              else [n]
              end
      # a name that is just digits is not a name: CRuby wants the Integer itself
      raise TypeError, "no implicit conversion of String into Integer" if n =~ /\A[0-9]+\z/
      cands.each do |cand|
        next unless cand =~ /\A[A-Z_][A-Za-z0-9_]*\z/
        return Socket.const_get(cand) if Socket.const_defined?(cand)
      end
      raise SocketError, "unknown UNIX control message: #{ty}"
    end

    def int = @data.unpack("i")[0]
    def cmsg_is?(level, type)
      @level == AncillaryData.__level(@family, level) &&
        @type == AncillaryData.__type(@family, @level, type)
    end
    def inspect = "#<Socket::AncillaryData: #{@family} #{@level} #{@type} #{@data.bytesize}bytes>"

    # IP_PKTINFO: struct in_pktinfo { int ifindex; in_addr spec_dst; in_addr addr; }
    def self.ip_pktinfo(addr, ifindex, spec_dst = addr)
      data = [ifindex].pack("i") +
             __in_addr(spec_dst) + __in_addr(addr)
      new(:INET, :IPPROTO_IP, :PKTINFO, data)
    end
    def ip_pktinfo
      return nil unless @family == Socket::AF_INET
      ifindex = @data.unpack1("i")
      spec = @data.byteslice(4, 4).unpack("C4").join(".")
      addr = @data.byteslice(8, 4).unpack("C4").join(".")
      [Addrinfo.ip(addr), ifindex, Addrinfo.ip(spec)]
    end

    # IPV6_PKTINFO: struct in6_pktinfo { in6_addr addr; int ifindex; }
    def self.ipv6_pktinfo(addr, ifindex)
      new(:INET6, :IPPROTO_IPV6, :PKTINFO, __in6_addr(addr) + [ifindex].pack("i"))
    end
    def ipv6_pktinfo
      return nil unless @family == Socket::AF_INET6
      [ipv6_pktinfo_addr, ipv6_pktinfo_ifindex]
    end
    def ipv6_pktinfo_addr
      return nil unless @family == Socket::AF_INET6
      groups = @data.byteslice(0, 16).unpack("n8").map { |g| g.to_s(16) }
      Addrinfo.ip(__compress6(groups))
    end
    def ipv6_pktinfo_ifindex
      return nil unless @family == Socket::AF_INET6
      @data.byteslice(16, 4).unpack1("i")
    end

    def self.__in_addr(a)
      ip = a.respond_to?(:ip_address) ? a.ip_address : a.to_s
      ip.split(".").map(&:to_i).pack("C4")
    end
    def self.__in6_addr(a)
      ip = a.respond_to?(:ip_address) ? a.ip_address : a.to_s
      # expand "::" then pack the 8 groups
      head, tail = ip.split("::", 2)
      hg = head.to_s.empty? ? [] : head.split(":")
      tg = tail.to_s.empty? ? [] : tail.split(":")
      groups = hg + Array.new(8 - hg.size - tg.size, "0") + tg
      groups = ip.split(":") if tail.nil?
      groups.map { |g| g.to_i(16) }.pack("n8")
    end
    def __compress6(groups)
      s = groups.join(":")
      s = s.sub(/\b(?:0:){2,}0\b/, ":") if s.include?("0:0")
      s.sub(/:{3,}/, "::")
    end
    private :__compress6

    def self.int(family, level, type, integer)
      new(family, level, type, [integer].pack("i"))
    end
    def self.unix_rights(*ios)
      new(:UNIX, :SOCKET, :RIGHTS, ios.map { |io| io.respond_to?(:fileno) ? io.fileno : io.to_int }.pack("i*"))
    end
    def unix_rights
      return nil unless @family == Socket::AF_UNIX && @level == Socket::SOL_SOCKET
      @data.unpack("i*").map { |fd| IO.for_fd(fd) }
    end
  end

  def initialize(family, type, protocol = 0)
    @family, @type = Socket.__family_strict(family), Socket.__socktype_strict(type)
    __init_fd(__sock_open(@family, @type, protocol), "r+b")
  end

  # BasicSocket#recvfrom answers the raw [af, port, host, addr] tuple; Socket's
  # own answers an Addrinfo (CRuby).
  def recvfrom(maxlen, flags = 0)
    mesg, addr = super
    [mesg, Addrinfo.__from_ary(addr)]
  end

  def recvfrom_nonblock(maxlen = 65536, flags = 0, outbuf = nil, exception: true)
    r = super
    return r if r.is_a?(Symbol)
    [r[0], Addrinfo.__from_ary(r[1])]
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

  # Like #accept, but hands back the raw descriptor instead of a Socket.
  def sysaccept
    loop do
      pair = __sock_accept(fileno)
      if pair
        nfd, addr = pair
        return [nfd, Addrinfo.__from_ary(addr)]
      end
      wait_readable
    end
  end

  def self.pair(family, type, protocol = 0)
    a, b = __sock_pair(__family_strict(family), __socktype_strict(type))
    [for_fd(a), for_fd(b)]
  end
  class << self; alias_method :socketpair, :pair; end

  def self.gethostname = __sock_hostname

  # getnameinfo(sockaddr, flags = 0) → [hostname, service].  The sockaddr may be
  # packed bytes, an Addrinfo, or the descriptive [family, port, host, addr].
  def self.getnameinfo(sa, flags = 0)
    if sa.is_a?(Array)
      # the family has to be one getnameinfo(3) can render; AF_UNIX has no
      # numeric host/service, so CRuby reports EAI_FAMILY
      fam = __family_strict(sa[0])
      unless fam == AF_INET || fam == AF_INET6 || fam == AF_UNSPEC
        raise ResolutionError.new("getnameinfo: ai_family not supported").tap { |e|
          e.instance_variable_set(:@error_code, EAI_FAMILY)
        }
      end
    end
    packed = sa.is_a?(String) ? sa : __pack(__unpack(sa))
    __sock_getnameinfo(packed, flags)
  end

  def self.getaddrinfo(host, service, family = nil, socktype = nil, protocol = nil,
                       flags = nil, reverse_lookup = nil, timeout: nil)
    rows = __sock_getaddrinfo(host&.to_s, service, family && __family(family),
                              socktype && __socktype(socktype), flags ? flags.to_int : 0)
    # reverse_lookup asks for the PTR name in the host slot instead of the
    # numeric address (the default is numeric).
    if reverse_lookup
      rows.each do |r|
        r[2] = begin
          getnameinfo([r[0], r[1], r[3], r[3]])[0]
        rescue StandardError
          r[3]
        end
      end
    end
    rows
  end

  # [canonical_name, aliases, address_family, *packed_addresses].
  # `<broadcast>` / `<any>` are named directly rather than resolved (CRuby).
  def self.gethostbyname(name)
    n = name.to_s
    case n
    when "<broadcast>" then return ["255.255.255.255", [], AF_INET, [255, 255, 255, 255].pack("C4")]
    when "<any>"       then return ["0.0.0.0",         [], AF_INET, [0, 0, 0, 0].pack("C4")]
    end
    h = __sock_hostent(n)
    [h[0], h[1], h[2], *h[3]]
  end

  # [hostname, aliases, address_family, packed_address]
  def self.gethostbyaddr(addr, family = nil)
    __sock_hostbyaddr(addr, family && __family(family))
  end

  # The port a named service listens on.
  def self.getservbyname(service, proto = "tcp")
    p = __sock_servbyname(service.to_s, proto.to_s)
    raise SocketError, "no such service #{service}/#{proto}" if p.nil?
    p
  end

  def self.getifaddrs
    []   # Socket::Ifaddr is not modelled; an empty list is honest, not a lie about interfaces
  end

  # Every interface address, as Addrinfos (Socket::Ifaddr is not needed here).
  def self.ip_address_list
    __sock_ifaddrs.map { |fam, addr| Addrinfo.ip(addr) }
  end

  # Listening sockets for every address `host`/`port` resolves to.  A port of 0
  # means "any", and CRuby gives every address the SAME port, so the first bind
  # picks it and the rest follow.
  def self.tcp_server_sockets(host_or_port, port = nil, &blk)
    host, port = port.nil? ? [nil, host_or_port] : [host_or_port, port]
    socks = []
    begin
      Addrinfo.getaddrinfo(host, port, nil, :STREAM, nil, Socket::AI_PASSIVE).each do |ai|
        s = Socket.new(ai.afamily, ai.socktype, ai.protocol)
        s.setsockopt(:SOCKET, :REUSEADDR, true)
        # without V6ONLY a wildcard IPv6 bind also claims the IPv4 port
        s.setsockopt(:IPV6, :V6ONLY, true) if ai.afamily == Socket::AF_INET6
        chosen = socks.empty? ? ai.ip_port : socks[0].local_address.ip_port
        s.bind(ai.family_addrinfo(ai.ip_address, chosen).to_sockaddr)
        s.listen(Socket::SOMAXCONN)
        socks << s
      end
    rescue Exception
      socks.each { |x| x.close unless x.closed? }
      raise
    end
    return socks unless blk
    begin
      blk.call(socks)
    ensure
      socks.each { |x| x.close unless x.closed? }
    end
  end

  # The service name a port belongs to.
  def self.getservbyport(port, proto = "tcp")
    n = __sock_servbyport(port.to_int, proto.to_s)
    raise SocketError, "no such service for port #{port}/#{proto}" if n.nil?
    n
  end

  # Socket.unix(path) → a Socket connected to a UNIX stream socket.
  def self.unix(path, &blk)
    s = Socket.new(:UNIX, :STREAM)
    begin
      s.connect(Socket.pack_sockaddr_un(path.to_s))
    rescue Exception
      s.close unless s.closed?
      raise
    end
    return s unless blk
    begin
      blk.call(s)
    ensure
      s.close unless s.closed?
    end
  end

  # A listening UNIX socket at `path`.
  def self.unix_server_socket(path, &blk)
    File.unlink(path) if File.exist?(path) && File.socket?(path)
    s = Socket.new(:UNIX, :STREAM)
    s.bind(Socket.pack_sockaddr_un(path))
    s.listen(Socket::SOMAXCONN)
    return s unless blk
    begin
      blk.call(s)
    ensure
      s.close unless s.closed?
      File.unlink(path) rescue nil
    end
  end

  # Bound (not listening) UDP sockets for host/port.
  def self.udp_server_sockets(host_or_port, port = nil, &blk)
    host, port = port.nil? ? [nil, host_or_port] : [host_or_port, port]
    socks = Addrinfo.getaddrinfo(host, port, nil, :DGRAM, nil, Socket::AI_PASSIVE).map do |ai|
      s = Socket.new(ai.afamily, ai.socktype, ai.protocol)
      s.bind(ai.to_sockaddr)
      s
    end
    return socks unless blk
    begin
      blk.call(socks)
    ensure
      socks.each { |s| s.close unless s.closed? }
    end
  end

  # Serve a UNIX socket at `path` forever, yielding [socket, client_addrinfo].
  def self.unix_server_loop(path, &blk)
    unix_server_socket(path) { |s| accept_loop(s, &blk) }
  end

  # Serve TCP on host/port forever, yielding [socket, client_addrinfo].
  def self.tcp_server_loop(host_or_port, port = nil, &blk)
    tcp_server_sockets(host_or_port, port) { |socks| accept_loop(socks, &blk) }
  end

  # Where a datagram came from, and how to answer it.
  class UDPSource
    attr_reader :remote_address, :local_address
    def initialize(remote, local, &reply) = (@remote_address, @local_address, @reply = remote, local, reply)
    def reply(mesg) = @reply.call(mesg)
    def inspect = "\#<Socket::UDPSource #{@remote_address.inspect} to #{@local_address.inspect}>"
  end

  # Read datagrams from already-bound sockets forever, yielding
  # [message, Socket::UDPSource].
  def self.udp_server_loop_on(sockets, &blk)
    socks = Array(sockets)
    loop do
      ready = IO.select(socks)&.first
      next if ready.nil?
      ready.each do |s|
        mesg, sender = s.recvfrom(65536)
        src = UDPSource.new(sender, s.local_address) { |reply| s.send(reply, 0, sender.to_sockaddr) }
        blk.call(mesg, src)
      end
    end
  end

  def self.udp_server_loop(host_or_port, port = nil, &blk)
    udp_server_sockets(host_or_port, port) { |socks| udp_server_loop_on(socks, &blk) }
  end

  # Accept forever from any of `sockets`, yielding [socket, addrinfo].
  def self.accept_loop(*sockets)
    socks = sockets.flatten
    loop do
      ready = IO.select(socks)&.first
      next if ready.nil?
      ready.each do |s|
        conn, addr = s.accept
        begin
          yield conn, addr
        ensure
          conn.close unless conn.closed?
        end
      end
    end
  end

  # Socket.tcp(host, port[, local_host, local_port]) → a connected Socket.
  def self.tcp(host, port, local_host = nil, local_port = nil, connect_timeout: nil, resolv_timeout: nil, &blk)
    remote = Addrinfo.tcp(host.to_s, port)
    if local_host
      remote.connect_from(local_host.to_s, local_port || 0, &blk)
    else
      remote.connect(&blk)
    end
  end

  # Socket#recvfrom / #accept exist on BasicSocket; the _nonblock forms differ
  # only in that they surface "would block" instead of parking.
  def accept_nonblock(exception: true)
    pair = __sock_accept(fileno)
    unless pair
      return :wait_readable unless exception
      raise IO::EAGAINWaitReadable, "accept(2) would block"
    end
    [Socket.for_fd(pair[0]), Addrinfo.__from_ary(pair[1])]
  end

  # nil family = infer from the host string, so sockaddr_in accepts IPv6 too.
  def self.sockaddr_in(port, host) = __sock_pack(nil, port, host.to_s)   # Integer or service name
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

  # Like __socktype but strict: an unknown name is a SocketError (CRuby).
  def self.__socktype_strict(t)
    t = t.to_int if !t.is_a?(Integer) && !t.is_a?(Symbol) && !t.is_a?(String) && t.respond_to?(:to_int)
    return t if t.is_a?(Integer)
    t = t.to_str if !t.is_a?(Symbol) && !t.is_a?(String) && t.respond_to?(:to_str)
    n = t.to_s.upcase.sub(/\ASOCK_/, "")
    { "STREAM" => SOCK_STREAM, "DGRAM" => SOCK_DGRAM, "RAW" => SOCK_RAW,
      "SEQPACKET" => SOCK_SEQPACKET, "RDM" => (defined?(SOCK_RDM) ? SOCK_RDM : 4) }.fetch(n) do
      raise SocketError, "unknown socket type: #{t}"
    end
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

  # Like __family but strict: an unknown name is a SocketError (CRuby).
  def self.__family_strict(f)
    f = f.to_int if !f.is_a?(Integer) && !f.is_a?(Symbol) && !f.is_a?(String) && f.respond_to?(:to_int)
    return f if f.is_a?(Integer)
    f = f.to_str if !f.is_a?(Symbol) && !f.is_a?(String) && f.respond_to?(:to_str)
    n = f.to_s.sub(/\AAF_|\APF_/, "")
    { "INET" => AF_INET, "INET6" => AF_INET6, "UNIX" => AF_UNIX,
      "LOCAL" => AF_UNIX, "UNSPEC" => AF_UNSPEC }.fetch(n.upcase) do
      raise SocketError, "unknown socket domain: #{f}"
    end
  end

  def self.__level(l)
    return l if l.is_a?(Integer)
    n = l.to_s.upcase.sub(/\ASOL_/, "")
    { "SOCKET" => SOL_SOCKET, "TCP" => IPPROTO_TCP, "IP" => IPPROTO_IP, "UDP" => IPPROTO_UDP }.fetch(n, SOL_SOCKET)
  end

  # Resolve an option name against its level's constant family.  Now that every
  # platform constant is defined, this is a lookup rather than a short table
  # with a silent default.
  def self.__optname(o, level = nil)
    raise TypeError, "no implicit conversion from nil to integer" if o.nil?
    o = o.to_int if !o.is_a?(Integer) && !o.is_a?(Symbol) && !o.is_a?(String) && o.respond_to?(:to_int)
    o = o.to_str if !o.is_a?(Integer) && !o.is_a?(Symbol) && !o.is_a?(String) && o.respond_to?(:to_str)
    return o if o.is_a?(Integer)
    n = o.to_s.upcase
    pre = case level
          when IPPROTO_TCP  then "TCP_"
          when IPPROTO_IP   then "IP_"
          when IPPROTO_UDP  then "UDP_"
          when (const_defined?(:IPPROTO_IPV6) ? IPPROTO_IPV6 : nil) then "IPV6_"
          else "SO_"
          end
    # only this level's own family: :CORK under :UDP must not find TCP_CORK
    [n, pre + n].each do |cand|
      next unless cand =~ /\A[A-Z_][A-Za-z0-9_]*\z/
      return const_get(cand) if const_defined?(cand) && const_get(cand).is_a?(Integer)
    end
    raise SocketError, "unknown socket level option name: #{o}"
  end

  # Like __level but strict: an unknown name is a SocketError.
  def self.__level_strict(l)
    raise TypeError, "no implicit conversion from nil to integer" if l.nil?
    l = l.to_int if !l.is_a?(Integer) && !l.is_a?(Symbol) && !l.is_a?(String) && l.respond_to?(:to_int)
    l = l.to_str if !l.is_a?(Integer) && !l.is_a?(Symbol) && !l.is_a?(String) && l.respond_to?(:to_str)
    return l if l.is_a?(Integer)
    n = l.to_s.upcase
    return SOL_SOCKET if n == "SOCKET" || n == "SOL_SOCKET"
    ["SOL_#{n}", "IPPROTO_#{n}", n].each do |cand|
      return const_get(cand) if const_defined?(cand) && const_get(cand).is_a?(Integer)
    end
    raise SocketError, "unknown protocol level: #{l}"
  end
end

# Addrinfo — one resolved endpoint.
class Addrinfo
  attr_reader :afamily, :pfamily, :socktype, :protocol

  # getaddrinfo rows are [famname, port, host, addr, afamily, socktype, protocol];
  # the 4-element form (from __sock_name / accept) has no trailing numbers.
  def self.__from_ary(a)
    ai = allocate
    if a.size >= 7
      ai.__setup(a[0], a[1], a[2], a[3], a[5] || Socket::SOCK_STREAM, a[6] || 0)
    else
      ai.__setup(a[0], a[1], a[2], a[3], a[4] || Socket::SOCK_STREAM, a[5] || 0)
    end
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
    if sockaddr.is_a?(Array)
      a = sockaddr
      af = Socket.__family_strict(a[0])
      # CRuby validates the Array form up front: the literal must be of the
      # declared family (an IPv4 address under AF_INET6 is an error).
      if af == Socket::AF_INET || af == Socket::AF_INET6
        lit = a[3].to_s
        ok = if af == Socket::AF_INET
               lit =~ /\A\d{1,3}(\.\d{1,3}){3}\z/
             else
               lit.include?(":")
             end
        raise Socket::ResolutionError, "getaddrinfo: Name or service not known" unless ok
      end
      if !family.nil?
        pf = Socket.__family_strict(family)
        unless pf == af || pf == Socket::PF_UNSPEC
          raise Socket::ResolutionError, "protocol family and address family are mismatched"
        end
      end
    else
      a = Socket.__unpack(sockaddr)
    end
    st = socktype.nil? ? 0 : Socket.__socktype_strict(socktype)
    if sockaddr.is_a?(Array) && (af == Socket::AF_INET || af == Socket::AF_INET6)
      # Let getaddrinfo(3) judge the family/socktype/protocol combination, which
      # is exactly what CRuby does — the accepted set is the platform's, not ours.
      # a raw socket has no service, and naming one makes getaddrinfo(3) refuse
      serv = (st == Socket::SOCK_RAW) ? nil : (a[1] || 0)
      __sock_getaddrinfo(a[3].to_s, serv, af, st, Socket::AI_NUMERICHOST, protocol || 0)
    end
    __setup(a[0], a[1] || 0, a[2], a[3] || a[2], st, protocol || 0)
    # CRuby: an explicit family wins; with a packed sockaddr String and no
    # family the pfamily is PF_UNSPEC, but the Array form takes its family.
    @pfamily = if !family.nil? then Socket.__family(family)
               elsif sockaddr.is_a?(Array) then @afamily
               else Socket::PF_UNSPEC
               end
    self
  end

  def self.getaddrinfo(host, service, family = nil, socktype = nil, protocol = nil, flags = nil)
    Socket.getaddrinfo(host, service, family, socktype, protocol, flags).map { |a| __from_ary(a) }
  end

  # Addrinfo.ip leaves socktype/protocol unspecified (0), unlike .tcp/.udp.
  def self.ip(host)
    r = getaddrinfo(host, nil, nil, Socket::SOCK_STREAM)
    raise SocketError, "getaddrinfo: no address for #{host}" if r.empty?
    a = r[0].to_a
    __from_ary([a[0], 0, a[2], a[3], 0, 0])   # .ip leaves socktype/protocol at 0
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

  # Hand a freshly connected socket to the block (closing it after) or back to
  # the caller.  CRuby always answers a Socket here, never a TCPSocket.
  private def __with_socket(s)
    return s unless block_given?
    begin
      yield s
    ensure
      s.close unless s.closed?
    end
  end

  # Build (host, port) / (path) / (Addrinfo) into an Addrinfo of this one's
  # socktype and protocol; a trailing options Hash has already been removed.
  private def __peer_like(args)
    return args[0] if args.size == 1 && args[0].is_a?(Addrinfo)
    return Addrinfo.unix(args[0].to_s) unless ip?
    Addrinfo.__from_ary([@famname, args[1] || 0, args[0].to_s, args[0].to_s, @socktype, @protocol])
  end

  # bind to `local` (when given), then connect to `remote`
  private def __connected_socket(remote, local)
    s = Socket.new(remote.afamily, remote.socktype, remote.protocol)
    begin
      s.bind(local.to_sockaddr) if local
      s.connect(remote.to_sockaddr)
    rescue Exception
      s.close unless s.closed?
      raise
    end
    s
  end

  def connect(*args, &blk)
    args.pop if args.last.is_a?(Hash)      # trailing options (:timeout) — not modelled
    __with_socket(__connected_socket(self, nil), &blk)
  end

  # CRuby answers a Socket here, not a TCPServer/UNIXServer.
  def bind(&blk)
    s = Socket.new(afamily, @socktype == 0 ? Socket::SOCK_STREAM : @socktype, @protocol)
    begin
      s.setsockopt(:SOCKET, :REUSEADDR, true) if ip?
      s.bind(to_sockaddr)
    rescue Exception
      s.close unless s.closed?
      raise
    end
    __with_socket(s, &blk)
  end

  def listen(backlog = Socket::SOMAXCONN, &blk)
    s = bind
    begin
      s.listen(backlog)
    rescue Exception
      s.close unless s.closed?
      raise
    end
    __with_socket(s, &blk)
  end

  # connect_to: self is the LOCAL address, the arguments name the peer.
  def connect_to(*args, &blk)
    args.pop if args.last.is_a?(Hash)
    remote = args.empty? ? self : __peer_like(args)
    __with_socket(__connected_socket(remote, args.empty? ? nil : self), &blk)
  end

  # connect_from: self is the PEER, the arguments name the local address to bind.
  def connect_from(*args, &blk)
    args.pop if args.last.is_a?(Hash)
    __with_socket(__connected_socket(self, args.empty? ? nil : __peer_like(args)), &blk)
  end

  # CRuby's shape: [afamily, address, pfamily, socktype, protocol, canonname, nil]
  # where `address` is [numeric_address, service] for IP and the path for UNIX.
  def marshal_dump
    addr = ip? ? [@addr.to_s, @port.to_s] : @host.to_s
    [afamily_name, addr, pfamily_name, socktype_name, protocol_name, canonname, nil]
  end

  def marshal_load(ary)
    fam = ary[0]
    if fam == "AF_UNIX"
      __setup(fam, 0, ary[1].to_s, ary[1].to_s, Socket.__socktype(ary[3] || 0), 0)
    else
      a = ary[1]
      __setup(fam, a[1].to_i, a[0].to_s, a[0].to_s,
              Socket.__socktype(ary[3] || 0), __protocol_of(ary[4]))
    end
  end

  def pfamily_name = afamily_name.to_s.sub(/\AAF_/, "PF_")

  def socktype_name
    case @socktype
    when Socket::SOCK_STREAM then "SOCK_STREAM"
    when Socket::SOCK_DGRAM  then "SOCK_DGRAM"
    when Socket::SOCK_RAW    then "SOCK_RAW"
    else nil
    end
  end

  def protocol_name
    case @protocol
    when Socket::IPPROTO_TCP then ip? ? "IPPROTO_TCP" : nil
    when Socket::IPPROTO_UDP then ip? ? "IPPROTO_UDP" : nil
    else nil
    end
  end

  def __protocol_of(name)
    case name
    when "IPPROTO_TCP" then Socket::IPPROTO_TCP
    when "IPPROTO_UDP" then Socket::IPPROTO_UDP
    else 0
    end
  end
  private :__protocol_of

  # "::ffff:1.2.3.4" (an IPv4-mapped IPv6 address) → the plain IPv4 Addrinfo.
  def ipv4_mapped? = ipv6_v4mapped?
  def ipv4_compat? = ipv6_v4compat?

  # The address as raw bytes (4 for IPv4, 16 for IPv6), or nil for a non-IP one.
  # Every classification predicate below is a plain bit test on these.
  private def __ip_bytes
    return nil unless ip?
    t = @addr.to_s.sub(/%.*\z/, "")                    # a zone id is not part of the address
    return t.split(".").map { |o| o.to_i }.pack("C4") if ipv4?
    groups = lambda do |part|
      out = []
      return out if part.nil? || part.empty?
      part.split(":").each do |g|
        next if g.empty?
        if g.include?(".")                             # a trailing dotted quad is the low 4 bytes
          out.concat(g.split(".").map { |o| o.to_i })
        else
          v = g.to_i(16)
          out << ((v >> 8) & 0xFF)
          out << (v & 0xFF)
        end
      end
      out
    end
    head, tail = t.split("::", 2)
    h = groups.call(head)
    return h.pack("C16") if tail.nil? && h.size == 16
    return nil if tail.nil?
    u = groups.call(tail)
    return nil if h.size + u.size > 16
    (h + [0] * (16 - h.size - u.size) + u).pack("C16")
  end

  private def __b(i) = (b = __ip_bytes) && b.getbyte(i)

  def ipv4_loopback?  = ipv4? && __b(0) == 127
  def ipv4_multicast? = ipv4? && (__b(0) & 0xF0) == 0xE0
  def ipv4_private?
    return false unless ipv4?
    b = __ip_bytes
    b.getbyte(0) == 10 ||
      (b.getbyte(0) == 172 && (b.getbyte(1) & 0xF0) == 16) ||
      (b.getbyte(0) == 192 && b.getbyte(1) == 168)
  end

  def ipv6_unspecified? = ipv6? && __ip_bytes == ("\0" * 16).b
  def ipv6_loopback?    = ipv6? && __ip_bytes == ("\0" * 15 + "\1").b
  def ipv6_multicast?   = ipv6? && __b(0) == 0xFF
  def ipv6_linklocal?   = ipv6? && __b(0) == 0xFE && (__b(1) & 0xC0) == 0x80
  def ipv6_sitelocal?   = ipv6? && __b(0) == 0xFE && (__b(1) & 0xC0) == 0xC0
  def ipv6_unique_local? = ipv6? && (__b(0) & 0xFE) == 0xFC
  def ipv6_v4mapped?
    return false unless ipv6?
    b = __ip_bytes
    b[0, 10] == ("\0" * 10).b && b.getbyte(10) == 0xFF && b.getbyte(11) == 0xFF
  end
  # ::a.b.c.d, but neither the unspecified nor the loopback address (CRuby)
  def ipv6_v4compat?
    return false unless ipv6?
    b = __ip_bytes
    return false unless b[0, 12] == ("\0" * 12).b
    (b.getbyte(12) << 24 | b.getbyte(13) << 16 | b.getbyte(14) << 8 | b.getbyte(15)) > 1
  end
  # multicast scope lives in the low nibble of the second byte
  private def __mc_scope(n) = ipv6? && __b(0) == 0xFF && (__b(1) & 0x0F) == n
  def ipv6_mc_nodelocal? = __mc_scope(1)
  def ipv6_mc_linklocal? = __mc_scope(2)
  def ipv6_mc_sitelocal? = __mc_scope(5)
  def ipv6_mc_orglocal?  = __mc_scope(8)
  def ipv6_mc_global?    = __mc_scope(0xE)

  def getnameinfo(flags = 0) = Socket.getnameinfo(to_sockaddr, flags)
  def ip_unpack = [ip_address, ip_port]
  def ipv6_to_ipv4
    return nil unless ipv4_mapped? || ipv4_compat?
    Addrinfo.__from_ary(["AF_INET", @port, @addr.to_s.split(":").last, @addr.to_s.split(":").last,
                         @socktype, @protocol])
  end

  # Build an Addrinfo of this one's family/socktype/protocol from (host, port)
  # for an IP address or (path) for a UNIX one; an Addrinfo passes through.
  def family_addrinfo(*args)
    return args[0] if args.size == 1 && args[0].is_a?(Addrinfo)
    if unix?
      raise ArgumentError, "wrong number of arguments (given #{args.size}, expected 1)" unless args.size == 1
      return Addrinfo.unix(args[0].to_s, @socktype == 0 ? Socket::SOCK_STREAM : @socktype)
    end
    raise ArgumentError, "wrong number of arguments (given #{args.size}, expected 2)" unless args.size == 2
    Addrinfo.__from_ary([@famname, args[1], args[0].to_s, args[0].to_s, @socktype, @protocol])
  end
  def afamily_name = @famname
end
