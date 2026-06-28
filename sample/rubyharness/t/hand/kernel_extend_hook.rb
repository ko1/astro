$log = []
module M
  def self.extended(obj); $log << [:extended, obj.class]; end
  def hello; "hi"; end
end
o = Object.new
o.extend(M)
p o.hello
p $log
p (begin; o.extend; rescue ArgumentError; "AE"; end)
module N; def n; 1; end; end
o.extend(N)
p o.n
