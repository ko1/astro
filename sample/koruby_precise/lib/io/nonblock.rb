# io/nonblock: IO#nonblock? / #nonblock= / #nonblock — no-op surface on a
# runtime whose sockets/pipes are already nonblocking under the hood.
class IO
  def nonblock? = true
  def nonblock=(v); v; end
  def nonblock(nb = true)
    block_given? ? yield(self) : self
  end
end
