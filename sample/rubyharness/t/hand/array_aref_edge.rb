big = 10 ** 20
r1 = (begin; [1,2,3][big]; rescue => e; e.class; end)
r2 = (begin; [1,2,3][0, big]; rescue => e; e.class; end)
r3 = (begin; [1,2,3][1..2, 3]; rescue => e; e.class; end)   # range + length → TypeError
r4 = [1,2,3][big] rescue "x"
p [r1, r2, r3]
