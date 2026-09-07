# #coerce が nil を返すと korb_try_coerce は korb_send_impl の後に handled=false で返る
class C; def coerce(o); nil; end; end
n = 0
200.times do
  begin; 1 + C.new;        rescue TypeError; n += 1; end
  begin; 1 - C.new;        rescue TypeError; n += 1; end
  begin; 1.0 * C.new;      rescue TypeError; n += 1; end
  begin; 1 & C.new;        rescue TypeError; n += 1; end
  begin; 1.div(C.new);     rescue TypeError; n += 1; end
end
puts "coerce_nil ok #{n}"
