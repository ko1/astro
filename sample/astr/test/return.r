# expect: 42
# explicit return
early <- function(x) {
    if (x > 10) {
        return(42)
    }
    return(0)
}
print(early(20))
