# method_added フックは「定義した側のフレーム」から呼ばれないといけない。
# alias / attr_* は node なので C から直接フックを撃っていて、リンクを渡して
# いなかった。その結果フックの中の caller_locations が空になり、test-unit の
# `caller_locations(1, 1)[0].absolute_path` が NoMethodError になっていた。
module H
  def method_added(name)
    l = caller_locations(1, 1)
    pth = l && l[0] && l[0].path
    pth = "(eval...)" if pth && pth.start_with?("(eval")   # eval の位置表記は別件の差分
    p [name, l && l.size, l && l[0] && l[0].lineno, pth && File.basename(pth)]
  end
  def singleton_method_added(name)
    l = caller_locations(1, 1)
    p [:s, name, l && l.size]
  end
end
class C
  extend H
  def a; end
  alias b a
  alias_method :c, :a
  attr_accessor :d
  attr_reader :r
  attr_writer :w
  define_method(:e) { }
  module_eval("def f; end")
end
module M2
  extend H
  def g; end
  alias h g
end
