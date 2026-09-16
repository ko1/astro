# The "other file" for cfile.rb: every C frame raised through here has its call
# site in THIS file, so the backtrace must name this file for it.
module CF
  module_function

  def each_raise
    [1].each { raise "boom" }
  end

  def each_raise_rescued
    [1].each { raise "boom" }
  rescue => e
    e.backtrace
  end

  def nested_c
    [1].each { [2].map { raise "boom" } }
  end

  def yielder
    [1].each { yield }
  end

  def direct
    [1].fetch(9)
  end

  def load_raiser
    require_relative 'cfile_raiser'
  end
end
