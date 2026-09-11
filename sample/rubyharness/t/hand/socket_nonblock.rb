# Socket *_nonblock never park: "would block" is IO::Wait{Readable,Writable} or a Symbol;
# a stream peer's orderly shutdown reads as nil. vs ruby.
require "socket"
require "tmpdir"
def try; yield; rescue Exception => e; [e.class, e.is_a?(IO::WaitReadable), e.is_a?(IO::WaitWritable)]; end

# recv_nonblock on an unbound / empty DGRAM socket
s1 = Socket.new(:INET, :DGRAM); s2 = Socket.new(:INET, :DGRAM)
p try { s1.recv_nonblock(1) }
s1.bind(Socket.pack_sockaddr_in(0, "127.0.0.1"))
p s1.recv_nonblock(5, exception: false)
s2.send("aaa", 0, s1.getsockname)
IO.select([s1], nil, nil, 2)
buf = "foo".force_encoding("ISO-8859-1")
p s1.recv_nonblock(5, 0, buf).equal?(buf), buf, buf.encoding
s2.send("bbb", 0, s1.getsockname)
IO.select([s1], nil, nil, 2)
buf = "foo".force_encoding("ISO-8859-1")
r = s1.recvfrom_nonblock(5, 0, buf)
p r[0].equal?(buf), buf, buf.encoding, r[1].class
s1.close; s2.close

# accept_nonblock on TCPServer / UNIXServer
srv = TCPServer.new("127.0.0.1", 0)
p try { srv.accept_nonblock }
p srv.accept_nonblock(exception: false)
c = TCPSocket.new("127.0.0.1", srv.addr[1])
IO.select([srv])
a = srv.accept_nonblock
p a.class
c.close; a.close; srv.close

Dir.mktmpdir do |d|
  path = File.join(d, "s.sock")
  us = UNIXServer.new(path)
  p try { us.accept_nonblock }, us.accept_nonblock(exception: false)
  uc = UNIXSocket.new(path)
  IO.select([us])
  ua = us.accept_nonblock
  p ua.class
  uc.write("hello"); p ua.recv(5)
  uc.close; ua.close; us.close
end

# connect_nonblock: EINPROGRESS is IO::WaitWritable, a connected socket answers EISCONN (or 0)
srv = TCPServer.new("127.0.0.1", 0)
addr = Socket.sockaddr_in(srv.addr[1], "127.0.0.1")
cl = Socket.new(:INET, :STREAM)
r = try { cl.connect_nonblock(addr) }
p(r == 0 || (r[0] <= Errno::EINPROGRESS && r[1..] == [false, true]))
IO.select(nil, [cl])
p try { cl.connect_nonblock(addr) }
p cl.connect_nonblock(addr, exception: false)
p try { cl.connect_nonblock(666) }
cl.close; srv.close

# a DGRAM connect_nonblock completes at once
d1 = Socket.new(:INET, :DGRAM); d2 = Socket.new(:INET, :DGRAM)
d1.bind(Socket.sockaddr_in(0, "127.0.0.1"))
p d2.connect_nonblock(d1.getsockname), d2.connect_nonblock(d1.connect_address)
d1.close; d2.close

# a stream peer's orderly shutdown reads as nil; an empty datagram as ""
srv = TCPServer.new("127.0.0.1", 0)
c = TCPSocket.new("127.0.0.1", srv.addr[1]); a = srv.accept
c.write("x"); IO.select([a]); p a.recv(10)
c.write("y"); IO.select([a]); p a.recvfrom(10)
c.write("z"); IO.select([a]); m = a.recvmsg; p m[0], m[1].afamily, m[1].pfamily, m[1].socktype, (m[1].ip_address rescue $!.class)
c.close; IO.select([a])
p a.recv(10), a.recv_nonblock(10, exception: false), a.recvfrom(10), a.recvmsg
a.close; srv.close
u1, u2 = UNIXSocket.pair(:DGRAM)
u1.send("", 0); p u2.recv(10)
u1.close; u2.close
