a = 1
b = 2
p proc { |p| eval("c = 3; binding.local_variables") }.call(4)
