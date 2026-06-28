e = [1, 2, 3].each
p e.next
p e.next
e.rewind
p e.next
p [1, 2, 3].each.next_values
p [1, 2, 3].each.peek_values
p({a: 1, b: 2}.each.next_values)
