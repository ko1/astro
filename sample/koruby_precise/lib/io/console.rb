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

  # IO::ConsoleMode: a copy of the terminal state
  def console_mode = IO::Console::Mode.__new(__tty_get)
  def console_mode=(mode)
    __tty_set(mode.__send__(:__state))
    mode
  end

  def ttyname = __tty_ttyname
  def input_pending? = __fionread > 0

  # VT100 sequences (console.c on a non-Windows terminal); each answers self
  def beep = (write("\a"); self)
  def goto(row, col) = (write("\e[#{Integer(row) + 1};#{Integer(col) + 1}H"); self)
  def goto_column(col) = (write("\e[#{Integer(col) + 1}G"); self)
  def cursor=(pos)
    row, col = pos
    goto(row, col)
  end
  def cursor_up(n) = __csi_move(n, "A", "B")
  def cursor_down(n) = __csi_move(n, "B", "A")
  def cursor_right(n) = __csi_move(n, "C", "D")
  def cursor_left(n) = __csi_move(n, "D", "C")
  def erase_line(mode) = (write("\e[#{Integer(mode)}K"); self)
  def erase_screen(mode) = (write("\e[#{Integer(mode)}J"); self)
  def clear_screen = (erase_screen(2); goto(0, 0))
  def scroll_forward(n) = __csi_move(n, "S", "T")
  def scroll_backward(n) = __csi_move(n, "T", "S")
  def hide_cursor = (write("\e[?25l"); self)
  def show_cursor = (write("\e[?25h"); self)
  private def __csi_move(n, fwd, back)
    n = Integer(n)
    write(n >= 0 ? "\e[#{n}#{fwd}" : "\e[#{-n}#{back}") unless n.zero?
    self
  end

  # the position the terminal reports for "\e[6n", 0-based [row, column]
  def cursor
    raw do
      write "\e[6n"
      flush
      resp = +""
      while (ch = getc)
        resp << ch
        break if ch == "R"
      end
      m = /\e\[(\d+);(\d+)R/.match(resp) or raise IOError, "unexpected cursor position response"
      [m[1].to_i - 1, m[2].to_i - 1]
    end
  end

  # Windows console only (CRuby raises the same on other platforms)
  %i[check_winsize_changed pressed? console_input_events].each do |m|
    define_method(m) { |*| raise NotImplementedError, "#{m}() function is unimplemented on this machine" }
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

# io/console's IO::generic_readable (included in StringIO): one character or a
# line, ignoring the tty options
class StringIO
  def getch(*) = getc
  # console.c io_getpass: prompt, gets, then a newline written to the same stream
  def getpass(prompt = nil)
    write(prompt) if prompt
    flush if respond_to?(:flush)
    begin
      str = gets
    ensure
      puts
    end
    str&.chomp
  end
end

# IO#console_mode's value: the termios bytes, edited through __termios_op
module IO::Console; end
class IO::Console::Mode
  def self.__new(state) = allocate.tap { |m| m.__send__(:__init, state) }
  private def __init(state) = (@state = state)
  private def __state = @state
  def initialize_copy(o) = (super; @state = o.__send__(:__state).dup)

  def echo = __termios_op(@state, 0, nil)
  alias echo? echo
  def echo=(flag)
    @state = __termios_op(@state, 1, flag)
  end
  def min = __termios_op(@state, 2, nil)
  def min=(n)
    @state = __termios_op(@state, 3, Integer(n))
  end
  def time = Rational(__termios_op(@state, 4, nil), 10)
  def time=(t)
    @state = __termios_op(@state, 5, (t * 10).to_i)
  end
  def raw!(min: nil, time: nil, intr: nil)
    @state = __termios_op(@state, 6, intr)
    self.min = min if min
    self.time = time if time
    self
  end
  def raw(**kw) = dup.raw!(**kw)
end
IO::ConsoleMode = IO::Console::Mode
