# String#delete/#count require an argument, except "".delete which returns "";
# delete! raises FrozenError on a frozen receiver. vs ruby.
p "".delete
p "abc".delete("b")
begin; "abc".delete; rescue ArgumentError; p :del_ae; end
begin; "".count; rescue ArgumentError; p :cnt_ae; end
p "hello".count("l")
begin; "x".freeze.delete!("x"); rescue FrozenError; p :frozen; end
p "aabbcc".delete("a-b")
