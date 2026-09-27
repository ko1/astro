# Dir#fileno's descriptor is closed with the Dir (tool/lib's leak checker reads /proc/self/fd that way).
def fds = Dir.children("/proc/self/fd").size
n0 = fds
3.times { Dir.open("/proc/self/fd") { |d| d.grep(/\A\d+\z/, &:to_i) - [d.fileno] } }
d = Dir.new("/tmp"); d.fileno; d.close
p fds - n0, (d.fileno rescue $!.class)
