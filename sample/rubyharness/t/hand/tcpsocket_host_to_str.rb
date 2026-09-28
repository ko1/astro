# TCPSocket.new converts a non-String host with #to_str (net-http's failure-message test
# raises from it)
require "socket"
host = Struct.new(:to_s).new("<example>")
def host.to_str; raise SocketError, "open failure"; end
begin
  TCPSocket.new(host, 1)
rescue SocketError => e
  p e.class, e.message
end
