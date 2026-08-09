# 純 Ruby MonitorMixin / Monitor — koruby の Thread::Mutex 上の再入 lock。
# (CRuby の monitor.so 相当を green-thread 前提で単純化)
module MonitorMixin
  class ConditionVariable
    def initialize(monitor)
      @monitor = monitor
      @cond = ::ConditionVariable.new
    end
    def wait(timeout = nil)
      @monitor.__send__(:mon_check_owner)
      count = @monitor.__send__(:mon_exit_for_cond)
      begin
        @cond.wait(@monitor.__send__(:mon_mutex), timeout)
        true
      ensure
        @monitor.__send__(:mon_enter_for_cond, count)
      end
    end
    def wait_while; wait while yield; end
    def wait_until; wait until yield; end
    def signal; @monitor.__send__(:mon_check_owner); @cond.signal; self; end
    def broadcast; @monitor.__send__(:mon_check_owner); @cond.broadcast; self; end
  end

  def self.extend_object(obj)
    super
    obj.__send__(:mon_initialize)
  end

  def mon_try_enter
    if @mon_owner == Thread.current
      @mon_count += 1
      true
    elsif @mon_mutex.try_lock
      @mon_owner = Thread.current
      @mon_count = 1
      true
    else
      false
    end
  end
  alias try_mon_enter mon_try_enter
  alias try_enter mon_try_enter

  def mon_enter
    if @mon_owner == Thread.current
      @mon_count += 1
    else
      @mon_mutex.lock
      @mon_owner = Thread.current
      @mon_count = 1
    end
    nil
  end
  alias enter mon_enter

  def mon_exit
    mon_check_owner
    @mon_count -= 1
    if @mon_count == 0
      @mon_owner = nil
      @mon_mutex.unlock
    end
    nil
  end
  alias exit mon_exit

  def mon_locked?; @mon_mutex.locked?; end
  def mon_owned?; @mon_owner == Thread.current; end

  def mon_synchronize
    mon_enter
    begin
      yield
    ensure
      mon_exit
    end
  end
  alias synchronize mon_synchronize

  def new_cond; ConditionVariable.new(self); end

  private

  def mon_initialize
    @mon_mutex = ::Thread::Mutex.new
    @mon_owner = nil
    @mon_count = 0
    self
  end

  def initialize(*args, &blk)
    super
    mon_initialize
  end

  def mon_check_owner
    raise ThreadError, "current thread not owner" unless @mon_owner == Thread.current
  end
  def mon_mutex; @mon_mutex; end
  def mon_exit_for_cond
    count = @mon_count
    @mon_owner = nil
    @mon_count = 0
    count
  end
  def mon_enter_for_cond(count)
    @mon_owner = Thread.current
    @mon_count = count
  end
end

class Monitor
  include MonitorMixin
  alias try_enter mon_try_enter
  alias enter mon_enter
  alias exit mon_exit
  # 上の alias は MonitorMixin 側の public alias と同じ (CRuby API 表面)
  def initialize; mon_initialize; end
end
