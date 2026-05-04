# expect: 29
# Ackermann's function — exercises 2-arg recursion
ack <- function(m, n) {
    if (m == 0) {
        n + 1
    } else if (n == 0) {
        ack(m - 1, 1)
    } else {
        ack(m - 1, ack(m, n - 1))
    }
}
print(ack(3, 2))
