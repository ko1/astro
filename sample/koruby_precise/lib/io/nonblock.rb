# io/nonblock: IO#nonblock? / #nonblock= / #nonblock.
#
# koruby keeps every socket and pipe O_NONBLOCK under the hood (the green-thread
# scheduler needs it), so #nonblock= cannot really put the descriptor back into
# blocking mode.  Remember the caller's intent instead, and fall back to the
# descriptor's own flag while nobody has expressed one — that is what the
# observable behaviour (File false, pipe/socket true) rests on.
require 'fcntl'

class IO
  def nonblock?
    v = @__nonblock
    return v unless v.nil?
    (fcntl(Fcntl::F_GETFL) & Fcntl::O_NONBLOCK) != 0
  end

  def nonblock=(v)
    @__nonblock = v ? true : false
    v
  end

  def nonblock(nb = true)
    return (self.nonblock = nb; self) unless block_given?
    prev = nonblock?
    self.nonblock = nb
    begin
      yield self
    ensure
      self.nonblock = prev
    end
  end
end
