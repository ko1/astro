# NoMethodError#name / #receiver + NameError#name. vs ruby.
obj = "hello"
begin
  obj.no_such_method
rescue NoMethodError => e
  p e.name
  p e.receiver.equal?(obj)
  p e.receiver
end
begin
  [1,2].totally_missing(3)
rescue NoMethodError => e
  p e.name
  p e.receiver
end
begin
  NonExistentConstant
rescue NameError => e
  p e.respond_to?(:name)
end
