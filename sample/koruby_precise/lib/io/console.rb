# io/console on termios of the IO's own descriptor (builtins/io.c __tty_*), so it
# works on a PTY slave that is not the process's controlling terminal.  A non-tty
# raises Errno::ENOTTY, as CRuby does.

class IO
  def raw(min: nil, time: nil, intr: nil)
    saved = __tty_get
    __tty_mode(0, intr)
    begin
      yield self
    ensure
      __tty_set(saved)
    end
  end

  def raw!(min: nil, time: nil, intr: nil)
    __tty_mode(0, intr)
    self
  end

  def cooked
    saved = __tty_get
    __tty_mode(1)
    begin
      yield self
    ensure
      __tty_set(saved)
    end
  end

  def cooked!
    __tty_mode(1)
    self
  end

  def echo=(flag)
    __tty_mode(flag ? 3 : 2)
    flag
  end

  def echo? = __tty_echo_p

  def noecho
    saved = __tty_get
    __tty_mode(2)
    begin
      yield self
    ensure
      __tty_set(saved)
    end
  end

  def getch(min: nil, time: nil, intr: nil)
    raw(intr: intr) { getc }
  end

  def getpass(prompt = nil)
    $stderr.print(prompt) if prompt
    line = noecho { gets }
    $stderr.puts
    line&.chomp
  end

  def winsize = __tty_winsize

  def winsize=(size)
    rows, cols = size
    __tty_winsize(Integer(rows), Integer(cols))
    size
  end

  def iflush; self; end
  def oflush; self; end
  def ioflush; self; end

  def IO.console(*)
    @__console = nil if @__console&.closed?
    @__console ||= begin
      File.open("/dev/tty", "r+")
    rescue StandardError
      nil
    end
  end
end
