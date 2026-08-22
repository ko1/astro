# PTY shim — no real pseudo-terminal yet.  PTY.spawn runs the child through a
# pipe (stderr merged), so readers get the output stream and a pid, but the
# child sees a pipe rather than a tty (isatty-sensitive programs act
# accordingly, e.g. line-buffer or drop colors).  Real openpty support would
# live in builtins/io.c; this unblocks PTY-using code paths until then.

module PTY
  class ChildExited < RuntimeError
    attr_reader :status
    def initialize(status = nil)
      @status = status
      super("pty - exited")
    end
  end

  def self.spawn(*command)
    r = IO.popen(command, err: [:child, :out])
    w = File.open(File::NULL, "w")
    if block_given?
      begin
        yield r, w, r.pid
      ensure
        r.close unless r.closed?
        w.close unless w.closed?
      end
    else
      [r, w, r.pid]
    end
  end

  class << self
    alias_method :getpty, :spawn
  end

  def self.check(pid, raise_on_exit = false)
    nil   # pipe children are reaped by IO.popen's close; nothing to poll
  end
end
