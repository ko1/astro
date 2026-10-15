# pack("P"/"p") pointers unpack only from the packed String (or a dup), short input → nil (2026-10-16)
def t(tag)
  r = yield
  puts "#{tag}: #{r.inspect}"
rescue Exception => e
  puts "#{tag}: #{e.class}: #{e.message}"
end
t(:same) { packed = ["hello"].pack("P"); [packed.unpack("P5"), packed.dup.unpack("P5"), packed.unpack("P1")] }
t(:rebuilt) { ["hello"].pack("p").to_sym.to_s.unpack("p") }
t(:nil_ptr) { [nil].pack("p").unpack("p") }
t(:short) { ["abc".unpack("aP"), "abc".unpack("aPa")] }
t(:no_ivars) { ["x"].pack("P").instance_variables }
t(:net_http) { require "net/http"; h = Net::HTTP::Get.new("/"); h["Content-Length"] = "123valid"; begin; h.content_length; rescue Net::HTTPHeaderSyntaxError => e; e.class; end }
