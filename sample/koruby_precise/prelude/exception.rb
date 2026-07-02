# Exception message helpers.
class Exception
  def detailed_message(highlight: false, **)
    m = message.to_s; cls = self.class; cn = cls.name
    if m.empty?
      text = cls.equal?(RuntimeError) ? 'unhandled exception' : (cn || cls.to_s)
      return highlight ? "\e[1;4m#{text}\e[m" : text
    end
    return (highlight ? "\e[1m#{m}\e[m" : m) if cn.nil?
    highlight ? "\e[1m#{m} (\e[1;4m#{cn}\e[m\e[1m)\e[m" : "#{m} (#{cn})"
  end
  def full_message(**opts); "#{message} (#{self.class})"; end
  # no arg → self; with a message → a new exception of the same class (CRuby clones + replaces).
  def exception(*args); args.empty? ? self : self.class.new(*args); end
  def self.exception(*args); new(*args); end   # Class-level Exception.exception(msg) == new(msg)
  # Equal iff same object, or same class + message + backtrace.
  def ==(other)
    return true if equal?(other)
    return false unless other.is_a?(Exception)
    self.class == other.class && message == other.message && backtrace == other.backtrace
  end
end
module Warning
  def self.[](category); false; end
  def self.[]=(category, flag); flag; end
  def self.warn(msg, category: nil); $stderr.print(msg) if $stderr; nil; end
end
class Object
  def to_enum(meth = :each, *args)
    a = []; send(meth, *args) { |*vs| a << (vs.size <= 1 ? vs[0] : vs) }; a.each
  end
  def enum_for(meth = :each, *args)
    a = []; send(meth, *args) { |*vs| a << (vs.size <= 1 ? vs[0] : vs) }; a.each
  end
end
# Minimal Errno: just enough that Errno::X constant references resolve (as
# SystemCallError subclasses); per-errno numbers/semantics are out of scope.
class SystemCallError < StandardError; end
module Errno
  EPERM = Class.new(SystemCallError); ENOENT = Class.new(SystemCallError)
  ESRCH = Class.new(SystemCallError); EINTR = Class.new(SystemCallError)
  EIO = Class.new(SystemCallError); EBADF = Class.new(SystemCallError)
  EAGAIN = Class.new(SystemCallError); ENOMEM = Class.new(SystemCallError)
  EACCES = Class.new(SystemCallError); EEXIST = Class.new(SystemCallError)
  ENOTDIR = Class.new(SystemCallError); EISDIR = Class.new(SystemCallError)
  EINVAL = Class.new(SystemCallError); EPIPE = Class.new(SystemCallError)
  ERANGE = Class.new(SystemCallError); ENOTSUP = Class.new(SystemCallError)
  ECHILD = Class.new(SystemCallError); ESPIPE = Class.new(SystemCallError)
  ECONNRESET = Class.new(SystemCallError); ETIMEDOUT = Class.new(SystemCallError)
  ENOTEMPTY = Class.new(SystemCallError); ENAMETOOLONG = Class.new(SystemCallError)
  ELOOP = Class.new(SystemCallError); EROFS = Class.new(SystemCallError)
  EXDEV = Class.new(SystemCallError); EMFILE = Class.new(SystemCallError)
  ENOSPC = Class.new(SystemCallError); EDOM = Class.new(SystemCallError)
end
