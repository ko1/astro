# String#tr/squeeze/delete/count raise ArgumentError on a descending range. vs ruby.
begin; "hello".tr("a-y", "z-a"); rescue => e; p e.class; end
begin; "hello".squeeze("m-a"); rescue => e; p e.class; end
begin; "hello".delete("z-a"); rescue => e; p e.class; end
begin; "hello".count("p-a"); rescue => e; p e.class; end
p "hello".tr("el", "ip")
p "hello".tr("a-y", "b-z")
p "aabbccdd".squeeze("a-c")
p "hello world".delete("l-o")
p "hello".count("a-z")
p "abc".tr("^a", "*")
