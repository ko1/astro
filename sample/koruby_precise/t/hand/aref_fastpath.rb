# node_aref/node_aset fast path (Array+fixnum) + deopt cases + redefinition.
a = [10, 20, 30]
p a[0]            # 10
p a[2]            # 30
p a[-1]           # 30
p a[99]           # nil
p a[-99]          # nil
a[1] = 99
p a               # [10, 99, 30]
a[-1] = 7
p a               # [10, 99, 7]

# 2-arg / range / hash / string → deopt to []
p [1, 2, 3, 4][1, 2]    # [2, 3]
p [1, 2, 3, 4][1..2]    # [2, 3]
p({ k: 5 }[:k])         # 5
p "hello"[1]            # "e"

# out-of-range assign grows (deopt to []=)
b = [1]
b[3] = 9
p b               # [1, nil, nil, 9]

# redefinition is honored (aref_redefined deopt)
class Array
  def [](i)
    "redef#{i}"
  end
end
p [100, 200][1]   # "redef1"
