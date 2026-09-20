# Date._parse の区切り: 月名の形は "-" も区切りに取る ("09-Nov-99")。
# 年の先頭の "-" は符号なので、区切りに食わせない ("1 Jan -4712")。
# 月名のあとの "." は略記の印だが、直後が年なら区切り ("5th.november.-2005")。
# asctime ("Thu Nov  9 23:12:40 1999") は年が時刻の後ろに来る。
require "date"
require "time"

[
  "09-Nov-99", "31-Dec-2010", "9-Nov-1999", "Nov-9-1999", "Jan-2010", "2010-Jan-9",
  "Wed, 09-Nov-99 23:12:40 GMT", "Friday, 31-Dec-2010 23:59:59 GMT",
  "09 Nov 99", "9 Nov 1999", "Nov 9, 1999", "Nov 1999", "Nov.2005", "5 Nov. 2005",
  "5th.november.2005", "5th.november.-2005", "5th/november/-2005",
  "1 Jan -4712", "Sat, 1 Jan 2000", "Thu Nov  9 23:12:40 1999",
  "1999-11-09", "11/09/1999", "09.11.1999", "-2010-12-31", "99-11-09",
  "2001-02-03T04:05:06+07:00", "Sun, 06 Nov 1994 08:49:37 GMT",
].each do |s|
  h = (Date._parse(s) rescue $!.class.to_s)
  # キーの並びは CRuby と違ってよい (Hash#== は順序を見ない) ので、ソートして出す
  puts "#{s}\t#{h.is_a?(Hash) ? h.sort_by { |k, _| k.to_s }.to_h.inspect : h}"
end

p Time.parse("Wednesday, 09-Nov-99 23:12:40 GMT").utc.to_s
p Time.parse("Friday, 31-Dec-2010 23:59:59 GMT").utc.to_s
p Date.parse("5th-november-2005").to_s

# Thread.new に「転送された C proc」(&method(:m) / &sym / &blk) を渡せる。
def watch(*) = "watched"
p Thread.start(&method(:watch)).value
pr = proc { |x = nil| 7 }
p Thread.start(&pr).value
def fwd(&b) = Thread.start(&b)
p fwd { 99 }.value
