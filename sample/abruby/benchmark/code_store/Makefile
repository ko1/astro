CC ?= gcc
CFLAGS ?= -O3 -fPIC -I/home/ko1/.rbenv/versions/4.0.2/include/ruby-4.0.0 -I/home/ko1/.rbenv/versions/4.0.2/include/ruby-4.0.0/x86_64-linux

SRCS = $(wildcard c/SD_*.c)
OBJS = $(patsubst c/%.c,o/%.o,$(SRCS))

all: all.so

all.so: $(OBJS)
	$(CC) -shared -o $@ $^

o/%.o: c/%.c | o
	$(CC) $(CFLAGS) -c $< -o $@

o:
	mkdir -p o

clean:
	rm -rf o all.so
