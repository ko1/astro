# Time#nsec / tv_nsec / tv_sec / tv_usec / usec. vs ruby.
t = Time.at(1500000000.5)
p t.tv_sec
p t.usec
p t.nsec
p t.tv_usec == t.usec
p t.tv_nsec == t.nsec
p (0..999_999_999).include?(t.nsec)
p Time.at(42).nsec
p Time.at(42).tv_sec
p Time.at(10.25).usec
