# raise cause: nil, Complex with user Numerics, inspect under a non-UTF-8 external,
# Refinement#instance_methods, import_methods of a C-backed module, ruby2_keywords Hash
# bound positionally, `public :m` following a later redefinition (2026-10-16)
def t(tag)
  r = yield
  puts "#{tag}: #{r.inspect}"
rescue Exception => e
  puts "#{tag}: #{e.class}: #{e.message}"
end
t(:cause_nil) {
  [->(e) { raise e, cause: nil }, ->(e) { raise e, "New", cause: nil }].map do |rr|
    begin
      begin
        begin; raise "Error 1"; rescue => e1; raise "Error 2"; end
      rescue => e2
        rr.call(e2)
      end
    rescue => e
      [e.message, e.cause&.message]
    end
  end
}
t(:cause_nil_fresh) { begin; begin; raise "a"; rescue; raise "b", cause: nil; end; rescue => e; e.cause; end }
class MockNum < Numeric
  def initialize(rv) = @rv = rv
  def real? = @rv
  def coerce(x) = [5, 2]
end
class RealM < Numeric; def ==(o) = o.is_a?(MockNum); end
t(:cpx_div) { Complex(3, 0) / MockNum.new(false) }
t(:cpx_eq) { [Complex(RealM.new, 0) == MockNum.new(true), Complex(3, 1) == MockNum.new(true)] }
t(:sjis_inspect) {
  ext = Encoding.default_external
  begin
    Encoding.default_external = Encoding::Windows_31J
    s = "\x87]".force_encoding("sjis")
    [s.inspect.b, s.to_sym.inspect.b, { s.to_sym => 1 }.inspect.b, s.inspect.encoding, "あ".inspect]
  ensure
    Encoding.default_external = ext
  end
}
t(:refine_im) { a = Array.instance_methods; b = nil; Module.new { refine(Array) { b = instance_methods } }; a == b }
t(:import_native) { require "zlib"; begin; Module.new { refine(String) { import_methods Zlib } }; rescue ArgumentError => e; e.message[/\A.*Zlib#/]; end }
class R2; ruby2_keywords def mark(*args) = args; def single(arg) = arg; def opt(a = 1) = a; def rest(*a) = a; end
t(:r2k_pos) {
  o = R2.new; args = o.mark(a: 1); m = args.last
  [o.single(*args), o.opt(*args), o.rest(*args).last].map { |r| [r.equal?(m), Hash.ruby2_keywords_hash?(r)] } + [Hash.ruby2_keywords_hash?(m)]
}
class VP; private def m = :before; end
class VC < VP; public :m; end
class VD < VC; end
class VP; def m = :after; end
t(:vis_clone) { [VC.new.m, VD.new.m, VC.public_method_defined?(:m), VP.private_method_defined?(:m)] }
