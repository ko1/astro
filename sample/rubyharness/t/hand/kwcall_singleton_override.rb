# A receiverless keyword call sees a method defined on the receiver's singleton
# after the site cached the class one (rspec-mocks stubbing a private
# send_request(command:) that rubocop calls with keywords).
class A; def run(x) = send_request(x); private def send_request(x) = :real; end
a = A.new
p a.run(1)
a.singleton_class.class_eval { define_method(:send_request) { |*x| :stub }; private :send_request }
p a.run(1)
b = A.new
def b.send_request(*) = :stub2
p b.run(1)
class A; def run2(command:) = send_request(command:); private def send_request(command:, args: []) = :real_kw; end
c = A.new
c.singleton_class.send(:define_method, :send_request) { |**k| :stub_kw }
p c.run2(command: 1)
class Base0; def run0 = send_request(command: 1); private; def send_request(command:, args: [], body: '') = :real; end
class Exec0 < Base0; def run = send_request(command: 'exec'); end
e = Exec0.new
p e.run
e.singleton_class.class_exec { define_method(:send_request) { |*a, **k, &b| :stub }; private :send_request }
p e.run, e.run0
f = Exec0.new
f.singleton_class.class_exec { define_method(:send_request) { |*a, &b| :stub_noks }; private :send_request }
p f.run
def m1(a = (def m1(x = 9) = [:new, x]; :d)) = [:old, a]
p m1, m1
def m2(a: (def m2(**) = :new2; :d), b: 1) = [:old2, a, b]
p m2(b: 3), m2(b: 4)
def m3(x, a: (def m3(*) = :new3; :d)) = [:old3, x, a]
p m3(1), m3(1)
o = Object.new
o.instance_eval "def m4(a: def m4(a: 1) a end, b:) [a, b] end"
p o.instance_eval { [m4(a: 2, b: 3), m4(b: 1), m4] }
