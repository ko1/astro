# An empty `**h` is no argument, also next to a block-pass `&blk` (a positional {} still is one).
def m0 = :ok
def m1(a) = a
def mr(*r) = r
def mk(**k) = k
def mb(&b) = b&.call
h = {}
pr = proc { :blk }
t = ->(&b) { b.call rescue "#{$!.class}: #{$!.message}" }
p [t.() { m0(**h, &nil) }, t.() { m0(**h, &pr) }, t.() { m1(1, **h, &pr) }, t.() { mr(**h, &pr) }, t.() { mr(1, **h, &nil) },
   t.() { mk(**h, &pr) }, t.() { mb(**h, &pr) }, t.() { m1(**h, &pr) }, t.() { send(:m0, **h, &pr) }, t.() { m0(*[], **h, &pr) }]
p [t.() { m0({}, &pr) }, t.() { mr({}, &pr) }, t.() { o = Object.new; def o.z = :z; o.z(**h, &pr) }, t.() { o = Object.new; def o.z = :z; o.z(**h, &nil) }, t.() { mr(**{}, &pr) }]
