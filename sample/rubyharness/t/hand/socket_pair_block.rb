# UNIXSocket.pair / Socket.pair with a block yield both halves and close them (Ruby 4.1)
require "socket"
p Socket.pair(:UNIX, :STREAM) { |a,b| [a.class, a.closed?] }
x=nil; p UNIXSocket.pair { |a,b| x=a; :v }, x.closed?
p((UNIXSocket.pair{|a,b| a.close; 1}))
p UNIXSocket.socketpair.map(&:class)
UNIXSocket.pair { |a, b| a.write "hi"; a.close_write; p b.read }
