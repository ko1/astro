# Assorted BasicSocket / Socket / Addrinfo details: argument coercion, error classes,
# address packing, IO#timeout, getifaddrs. vs ruby.
require "socket"
def try; yield; rescue Exception => e; [e.class, e.message]; end

# send: the message answers #to_str
o = Object.new; def o.to_str; "hello"; end
u1 = UDPSocket.new; u1.bind("127.0.0.1", 0); u2 = UDPSocket.new
p u2.send(o, 0, u1.local_address.to_sockaddr), u1.recvfrom(10)[0]
p try { u2.send(Object.new, 0) }
u1.close; u2.close

# setsockopt(Socket::Option) with extra arguments; shutdown / Socket.new argument types
s = Socket.new(:INET, :STREAM)
opt = Socket::Option.int(:INET, :SOCKET, :KEEPALIVE, 1)
p s.setsockopt(opt), try { s.setsockopt(opt, 1) }, try { s.setsockopt(opt, 1, 2) }
p try { s.shutdown(Object.new) }
p try { Socket.new(:INET, :STREAM, :TCP) }
bad = Object.new; def bad.to_str; 1; end
p try { Socket.new(bad, :STREAM) }, try { Socket.pair(bad, :STREAM) }
s.close

# IPSocket#inspect
srv = TCPServer.new("127.0.0.1", 0)
p srv.inspect.sub(/fd \d+/, "fd N").sub(/\d+>\z/, "PORT>")
srv.close; p srv.inspect

# getaddrinfo: protocol, AI_CANONNAME, #to_str hosts
p Addrinfo.getaddrinfo("127.0.0.1", 80, nil, nil, Socket::IPPROTO_UDP).first.then { |a| [a.socktype, a.protocol] }
p Addrinfo.getaddrinfo("localhost", 80, nil, :STREAM, nil, Socket::AI_CANONNAME).map(&:canonname).uniq.map(&:class)
h = Object.new; def h.to_str; "127.0.0.1"; end
p Socket.getaddrinfo(h, 80, :INET, :STREAM).map { |r| r[3] }.uniq
p try { Socket.getaddrinfo(bad, 80) }

# getnameinfo argument checks; sockaddr packing
p try { Socket.getnameinfo("cats") }.first, try { Socket.getnameinfo(["AF_INET"]) }, try { Socket.getnameinfo(["AF_INET", 80]) }
p Socket.unpack_sockaddr_in(Socket.sockaddr_in(0, ""))
p ["127.0.0.1", "::1"].include?(Socket.unpack_sockaddr_in(Socket.sockaddr_in(0, nil))[1])
p Socket.unpack_sockaddr_in(Socket.sockaddr_in(80, "<broadcast>")), Socket.unpack_sockaddr_in(Socket.sockaddr_in(80, "<any>"))
p try { Socket.sockaddr_un("a" * 109) }, Socket.sockaddr_un("a" * 108).bytesize
p Addrinfo.new(["AF_INET", 0, "", ""]).to_sockaddr == Socket.sockaddr_in(0, "")
p try { Addrinfo.new(["AF_INET", 80, "x", "127.0.0.1"], nil, nil, Socket::IPPROTO_TCP) }.first
p Addrinfo.new(["AF_INET", 80, "x", "127.0.0.1"], nil, nil, Socket::IPPROTO_UDP).protocol
p try { TCPServer.new("foo") }, try { TCPSocket.new("127.0.0.1", "foo") }

# TCPSocket.new(nil, port) reaches a server on the loopback
srv = TCPServer.new("127.0.0.1", 0)
c = TCPSocket.new(nil, srv.addr[1]); p c.peeraddr[3]; c.close; srv.close

# close_read / close_write on a closed socket (a plain IO answers nil)
srv = TCPServer.new("127.0.0.1", 0); srv.close
p try { srv.close_read }, try { srv.close_write }
r, w = IO.pipe; r.close; p r.close_read; w.close

# UDP: an oversized datagram is EMSGSIZE from #write and #send alike
u = UDPSocket.new; u.connect("127.0.0.1", 9)
p try { u.write("a" * 100000) }.first, try { u.send("a" * 100000, 0) }.first
u.close

# IO#timeout
sk = Socket.new(:INET, :STREAM); p sk.timeout; sk.timeout = 1.5; p sk.timeout; sk.timeout = nil; p sk.timeout
p try { sk.timeout = -1 }, try { sk.timeout = "x" }
sk.timeout = 0
p try { sk.connect(Socket.pack_sockaddr_in(1, "192.0.2.1")) }.first   # TEST-NET-1: nothing answers
sk.close

# getifaddrs
ifs = Socket.getifaddrs
p ifs.class, ifs.empty?, ifs.all? { |i| i.is_a?(Socket::Ifaddr) && i.name.is_a?(String) && i.ifindex.is_a?(Integer) && i.flags.is_a?(Integer) }
p ifs.map(&:addr).compact.all? { |a| a.is_a?(Addrinfo) && a.afamily != Socket::AF_UNSPEC }
p ifs.map(&:netmask).compact.select(&:ip?).all? { |a| a.ip_address.is_a?(String) }
p Socket.ip_address_list.all? { |a| a.is_a?(Addrinfo) && a.ip? }, Socket.ip_address_list.any? { |a| a.ip_address == "127.0.0.1" }
