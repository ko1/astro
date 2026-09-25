# raise "str": the message IS that String (encoding kept, NULs kept, same object).
s = "\xC2\xA9".b
begin; raise s; rescue => x; p x.message.encoding, x.message.equal?(s), x.class; end
begin; raise "a\0b"; rescue => x; p x.message, x.full_message(highlight: false).include?("a\u0000b"); end
f = "frozen".freeze
begin; raise f; rescue => x; p x.message.frozen?, x.message.equal?(f); end
class MyS < String; end
begin; raise MyS.new("sub"); rescue => x; p x.message.class, x.message; end
begin; raise "é"; rescue => x; p x.message.encoding, x.inspect; end
