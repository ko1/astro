p nil.singleton_class
p true.singleton_class
p false.singleton_class
p (begin; 5.singleton_class; rescue TypeError; "TE"; end)
p (begin; :s.singleton_class; rescue TypeError; "TE"; end)
class C; end
o = C.new
p o.send(:initialize_copy, C.new).equal?(o)
p (begin; o.send(:initialize_copy, "str"); rescue TypeError; "TE"; end)
