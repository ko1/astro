# fcntl: the F_* / O_* constants Fcntl exposes.
module Fcntl
  F_DUPFD = 0; F_GETFD = 1; F_SETFD = 2; F_GETFL = 3; F_SETFL = 4
  F_GETLK = 5; F_SETLK = 6; F_SETLKW = 7
  FD_CLOEXEC = 1
  O_RDONLY = 0; O_WRONLY = 1; O_RDWR = 2; O_ACCMODE = 3
  O_CREAT = 0100; O_EXCL = 0200; O_NOCTTY = 0400; O_TRUNC = 01000
  O_APPEND = 02000; O_NONBLOCK = 04000; O_NDELAY = O_NONBLOCK
end
