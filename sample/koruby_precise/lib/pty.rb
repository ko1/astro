# PTY — a real pseudo-terminal (IO.__openpty in builtins/io.c).  The child gets
# the slave as stdin/stdout/stderr, so isatty-sensitive programs (irb, reline)
# behave as they do under CRuby.  The child does not become a session leader,
# so it has no controlling terminal (/dev/tty is the parent's).

module PTY
  class ChildExited < RuntimeError
    attr_reader :status
    def initialize(status = nil)
      @status = status
      super("pty - exited: #{status&.pid}")
    end
  end

  def self.open
    master, _, sfd, path = IO.send(:__openpty, true)
    slave = File.for_fd(sfd, "r+", path: path)
    slave.sync = true
    pair = [master, slave]
    return pair unless block_given?
    begin
      yield pair
    ensure
      master.close unless master.closed?
      slave.close unless slave.closed?
    end
  end

  def self.spawn(*command)
    env = command.first.is_a?(Hash) ? command.shift : {}
    command = [ENV["SHELL"] || "/bin/sh"] if command.empty?
    r, w, sfd, _path = IO.send(:__openpty)
    pid = begin
      Process.spawn(env, *command, in: sfd, out: sfd, err: sfd)
    ensure
      IO.for_fd(sfd).close
    end
    return [r, w, pid] unless block_given?
    begin
      yield r, w, pid
    ensure
      r.close unless r.closed?
      w.close unless w.closed?
    end
  end

  class << self
    alias_method :getpty, :spawn
  end

  def self.check(pid, raise_on_exit = false)
    got = Process.waitpid(pid, Process::WNOHANG | Process::WUNTRACED)
    return nil unless got
    st = $?
    raise ChildExited.new(st) if raise_on_exit
    st
  end
end
