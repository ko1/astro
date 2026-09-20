# Builtin #arity must match CRuby: rake's FileList delegation picks the call
# shape from `[].method(m).arity`, so a variadic stand-in silently changes it.
%w[map collect sort sort_by select find_all reject grep grep_v compact flatten
   uniq values_at combination repeated_combination repeated_permutation
   permutation pack reverse_each + - & |].each do |m|
  puts "Array##{m}\t#{Array.instance_method(m).arity}"
end
%w[unpack unpack1].each { |m| puts "String##{m}\t#{String.instance_method(m).arity}" }
puts "Module#set_temporary_name\t#{Module.instance_method(:set_temporary_name).arity}"
puts "Enumerable#reverse_each\t#{Enumerable.instance_method(:reverse_each).arity}"
puts "Enumerable#to_set\t#{Enumerable.instance_method(:to_set).arity}"

# the fixed arities are enforced
[["grep", []], ["|", []], ["combination", []], ["combination", [1, 2]]].each do |m, args|
  begin
    [1, 2].send(m, *args)
    puts "#{m}(#{args.size}) ok"
  rescue ArgumentError => e
    puts "#{m}(#{args.size}) #{e.message}"
  end
end
p([1, 2] | [2, 3])
p [1, 2, 3].grep(2..3)
p [1, 2].combination(1).to_a
