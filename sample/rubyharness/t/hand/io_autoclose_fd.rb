# autoclose = false detaches #close from the descriptor: webrick's
# create_listeners hands the fd to TCPServer.for_fd and then closes the Socket.
require 'socket'

r, w = IO.pipe
w2 = IO.for_fd(w.fileno)
w.autoclose = false
p w.autoclose?
w.close
p w.closed?
w2.write("x")
w2.flush
p r.read(1)
w2.close
r.close

s = Socket.tcp_server_sockets("127.0.0.1", 0).first
s.autoclose = false
ts = TCPServer.for_fd(s.fileno)
s.close
p ts.addr.first
p ts.addr[2]
ts.close
