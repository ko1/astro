# An arity error names the CALLEE: its `def` line is the top backtrace frame,
# and a required keyword is spelled out in the message.
class A
  def zero                     # 4
  end
  def kw(a:)                   # 6
  end
  def kw2(x, k1:, k2: 2)       # 8
  end
  def rest(x, *r, k:)          # 10
  end
  def opt(a, b = 1)            # 12
  end
end

def show
  yield
rescue ArgumentError => e
  puts "#{e.message} | #{e.backtrace[0].sub(__dir__ + "/", "")}"
end

a = A.new
show { a.zero(1) }
show { a.send(:zero, 1) }
show { a.kw }
show { a.kw(1) }
show { a.kw2(1, 2) }
show { a.kw2(1) }
show { a.rest }
show { a.opt }
show { a.opt(1, 2, 3) }
show { A.instance_method(:zero).bind(a).call(1) }
show { a.method(:zero).call(1) }
show { [[1]].each { |x| a.zero(x) } }

# the second call takes the filled inline cache: same answer
2.times { show { a.zero(1) } }

# unknown keywords name the callee too
show { a.kw(a: 1, b: 2) }
show { a.kw2(1, k1: 1, nope: 2) }
show { a.kw2(1, k1: 1, **{"str" => 2}) }
