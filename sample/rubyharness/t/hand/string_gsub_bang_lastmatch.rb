# gsub!/sub!/slice!: $~ keeps the pre-mutation text; a block that resizes self
# during sub!/gsub! is a RuntimeError; gsub on a broken UTF-8 string raises.
'hello.'.gsub!('l', 'l' => 'L'); p $~.begin(0), $~[0], $~.string, $~.string.frozen?
'hello.'.sub!('l', 'l' => 'L'); p $~.begin(0), $~[0]
'hello.'.gsub!(/.(.)/, 'o' => ' hole'); p $~[0], $~[1]
'hello.'.gsub!(/not/, 'z' => 'g'); p $~
s = "hello"; s.gsub!(/l/) { "L" }; p s, $~[0], $~.pre_match
s = 'hello'; p s.slice!(/./), $~[0], $~.post_match, s
s = 'hello'; p s.slice!(/l+/), $~[0], $~.pre_match, s
'hello'.slice!(/not/); p $~
str = "hello"
begin; str.sub!(/l/) { str << "x"; "y" }; rescue RuntimeError => e; puts "RuntimeError: #{e.message}"; end
str = "hello"
begin; str.gsub!(/l/) { str.replace("hi"); "y" }; rescue RuntimeError => e; puts "RuntimeError: #{e.message}"; end
str = "hello"; p str.sub(//) { str[0] = 'x' }, str
x92 = [0x92].pack('C').force_encoding('utf-8')
[->{ "a#{x92}b".gsub(/[^\x00-\x7f]/u, '') }, ->{ "a#{x92}b".gsub(/x/) { '' } }, ->{ "a#{x92}b".sub!("a", "b") }].each do |l|
  begin; p l.call; rescue ArgumentError => e; puts "ArgumentError: #{e.message}"; end
end
