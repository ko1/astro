# Kernel#printf(fmt, *args) writes sprintf output to stdout. vs ruby.
printf("%d and %s\n", 42, "str")
printf("%05.2f\n", 3.14159)
printf("%x %o %b\n", 255, 8, 5)
printf("no format args\n")
printf("%-10s|\n", "left")
