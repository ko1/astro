# io/console shim on stty(1) — koruby has no termios binding yet.  Covers the
# common editor surface: raw / raw! / cooked / cooked!, echo, getch, winsize,
# IO.console.  Terminal modes are set with `stty` against the controlling
# terminal, so this works only where /dev/tty and stty(1) exist (i.e. a real
# terminal on a POSIX host); calls degrade to no-ops when they don't.

class IO
  def raw(*)
    saved = IO.__console_stty("-g")
    IO.__console_stty("raw -echo")
    begin
      yield self
    ensure
      IO.__console_stty(saved) if saved
    end
  end

  def raw!(*)
    IO.__console_stty("raw -echo")
    self
  end

  def cooked
    saved = IO.__console_stty("-g")
    IO.__console_stty("sane")
    begin
      yield self
    ensure
      IO.__console_stty(saved) if saved
    end
  end

  def cooked!
    IO.__console_stty("sane")
    self
  end

  def echo=(flag)
    IO.__console_stty(flag ? "echo" : "-echo")
    flag
  end

  def echo?
    out = IO.__console_stty("-a")
    out ? !out.include?("-echo ") : true
  end

  def getch(*)
    raw { getc }
  end

  def noecho
    saved = IO.__console_stty("-g")
    IO.__console_stty("-echo")
    begin
      yield self
    ensure
      IO.__console_stty(saved) if saved
    end
  end

  def winsize
    out = IO.__console_stty("size")
    if out && out =~ /\A(\d+)\s+(\d+)/
      [$1.to_i, $2.to_i]
    else
      rows = ENV["LINES"].to_i
      cols = ENV["COLUMNS"].to_i
      [rows > 0 ? rows : 24, cols > 0 ? cols : 80]
    end
  end

  def winsize=(size)
    rows, cols = size
    IO.__console_stty("rows #{rows.to_i} cols #{cols.to_i}")
    size
  end

  def iflush; self; end
  def oflush; self; end
  def ioflush; self; end

  # Run stty against the controlling terminal; nil when there is none.
  def IO.__console_stty(args)
    out = IO.popen("stty #{args} < /dev/tty 2> /dev/null", err: File::NULL) { |io| io.read }
    $?.success? ? out.chomp : nil
  rescue StandardError
    nil
  end

  def IO.console
    @__console ||= begin
      File.open("/dev/tty", "r+")
    rescue StandardError
      nil
    end
  end
end
