# ENV: aliases, block-less enumerators, #inspect, #key/#rassoc #to_str
# coercion, #value? nil, empty-name EINVAL, #fetch warning, #delete coercion.
ENV["KORB_ENV_MISC"] = "bar"
p ENV.method(:each) == ENV.method(:each_pair)
p ENV.method(:has_value?) == ENV.method(:value?)
p ENV.each_key.class, ENV.each_value.class, ENV.each_pair.class
p ENV.each_key.to_a.include?("KORB_ENV_MISC")
p ENV.inspect.include?('"KORB_ENV_MISC" => "bar"')
p ENV.to_s

k = Object.new
def k.to_str = "bar"
p ENV.key(k)
p ENV.rassoc(k)
p ENV.rassoc(Object.new)
p ENV.rassoc(42)
begin
  ENV.key(Object.new)
rescue TypeError => e
  puts e.message
end
p ENV.value?(Object.new)
p ENV.value?("bar")

begin
  ENV[""] = "x"
rescue Errno::EINVAL => e
  puts "EINVAL"
end
begin
  ENV.merge!("" => "x")
rescue Errno::EINVAL => e
  puts "EINVAL merge!"
end
ENV["foo="] = nil
p ENV.key?("foo=")

$VERBOSE = true
orig = $stderr
r, w = IO.pipe
$stderr = w
p ENV.fetch("KORB_ENV_MISC_NONE", "default") { "blk" }
$stderr = orig
w.close
puts r.read.sub(/\A.*?warning: /, "warning: ")

kk = Object.new
$calls = 0
def kk.to_str
  $calls += 1
  "KORB_ENV_MISC"
end
ENV.delete(kk)
p $calls, ENV["KORB_ENV_MISC"]

begin
  ENV.to_h { |k, v| Object.new }
rescue TypeError => e
  puts e.message
end
