# Socket::Option.ipv4_multicast_loop / _ttl and their readers (rinda's
# RingFinger); a UDP socket takes the option and reports it back.
require 'socket'
o = Socket::Option.ipv4_multicast_loop(1); p o.ipv4_multicast_loop, o.data, o.level, o.optname, o.family
o = Socket::Option.ipv4_multicast_ttl(2); p o.ipv4_multicast_ttl, o.data
p((Socket::Option.int(:INET, :SOCKET, :KEEPALIVE, 1).ipv4_multicast_ttl rescue $!))
p((Socket::Option.new(:INET, :IP, :MULTICAST_TTL, "\x01").ipv4_multicast_ttl rescue $!))
p((Socket::Option.ipv4_multicast_loop(true) rescue $!.class))
s = UDPSocket.new
s.setsockopt(Socket::Option.ipv4_multicast_loop(1))
s.setsockopt(Socket::Option.ipv4_multicast_ttl(3))
p s.getsockopt(:IPPROTO_IP, :IP_MULTICAST_TTL).ipv4_multicast_ttl, s.getsockopt(:IPPROTO_IP, :IP_MULTICAST_LOOP).ipv4_multicast_loop
s.close
p [(Socket::Option.new(:INET, :SOCKET, :KEEPALIVE, "\x01").int rescue $!), Socket::Option.new(:INET, :SOCKET, :KEEPALIVE, "\x01").bool]
p((Socket::Option.new(:INET, :SOCKET, :KEEPALIVE, "\x01\x00").bool rescue $!))
