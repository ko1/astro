# Classic recursive fib benchmark.  Sized so the run completes in
# ~1 second on a modern x86_64 with the interpreter (and well under
# that with -c).
fib <- function(n) {
    if (n < 2) {
        n
    } else {
        fib(n - 1) + fib(n - 2)
    }
}
print(fib(36))
