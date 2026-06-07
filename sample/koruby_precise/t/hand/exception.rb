# L1: exceptions — raise / rescue / ensure / retry / custom
begin
  raise "boom"
rescue => e
  p e.message
  p e.class
end

begin
  raise ArgumentError, "bad arg"
rescue ArgumentError => e
  p e.message
  p e.class
end

begin
  1 / 0
rescue ZeroDivisionError => e
  p "caught zerodiv"
end

begin
  [].fetch(5)
rescue IndexError => e
  p "caught index"
end

begin
  {}.fetch(:missing)
rescue KeyError => e
  p "caught key"
end

begin
  Integer("notanumber")
rescue ArgumentError
  p "caught int parse"
end

begin
  nil.upcase
rescue NoMethodError => e
  p "caught nomethod"
end

def risky(x)
  raise "negative" if x < 0
  x * 2
end
begin
  risky(-1)
rescue => e
  p e.message
end
p risky(5)

result = begin
  raise "x"
rescue
  "rescued value"
end
p result

order = []
begin
  order << :try
  raise "e"
rescue
  order << :rescue
ensure
  order << :ensure
end
p order

ens = []
def cleanup(ens)
  ens << :body
  return :returned
ensure
  ens << :ensure
end
p cleanup(ens)
p ens

attempts = 0
begin
  attempts += 1
  raise "fail" if attempts < 3
  p "succeeded after #{attempts}"
rescue
  retry if attempts < 3
end

class MyError < StandardError
  def initialize(msg = "custom default")
    super
  end
end
begin
  raise MyError
rescue MyError => e
  p e.message
end
begin
  raise MyError, "explicit"
rescue StandardError => e
  p e.message
  p e.is_a?(MyError)
end

begin
  raise TypeError, "t"
rescue ArgumentError
  p "wrong"
rescue TypeError => e
  p "right: #{e.message}"
end

x = begin
  Integer("42")
rescue ArgumentError
  -1
end
p x

p (raise "inline" rescue "saved")
