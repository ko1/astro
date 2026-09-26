# Encoding.find remembers what it resolved (rspec calls it thousands of times):
# names, aliases and case all resolve as before, unknown names still raise, and
# Encoding keeps no visible class variables.
%w[utf-8 UTF-8 binary BINARY ascii US-ASCII eucJP EUC-JP CP932 Shift_JIS locale external filesystem].each do |n|
  2.times { print Encoding.find(n).name, " " }
end
puts
p Encoding.find(Encoding::ISO_8859_1), Encoding.find("iso-8859-1")
p (Encoding.find("nope") rescue $!.message), (Encoding.find("nope") rescue $!.class), (Encoding.find(:utf8) rescue $!.class)
p Encoding.class_variables
o = Object.new; def o.to_str = "utf-8"; p Encoding.find(o)
