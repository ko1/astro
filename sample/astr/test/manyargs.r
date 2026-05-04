# expect: 36
# 6 args — exercises node_call_n (>3 arity)
combine <- function(a, b, c, d, e, f) {
    a + b + c + d + e + f
}
print(combine(1, 2, 3, 4, 5, 21))
