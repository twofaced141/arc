CC       = gcc
CFLAGS   = -ffreestanding -nostdlib -nostartfiles -nodefaultlibs \
           -m32 -Wall -Wextra -std=c17 \
           -fno-pic -no-pie -fno-stack-protector \
           -fno-asynchronous-unwind-tables -mno-sse -mno-mmx -O0 \
           -fno-builtin -fno-omit-frame-pointer \
           -I include -I ../../include
AR       = ar
ARFLAGS  = rcs

SRCS   = string.c stdio.c stdlib.c ctype.c errno.c unistd.c
OBJS   = $(SRCS:.c=.o)
LIBC   = libc.a
CRT0_O = crt0.o

.PHONY: all clean

all: $(LIBC) $(CRT0_O)

$(LIBC): $(OBJS)
	$(AR) $(ARFLAGS) $@ $^

$(CRT0_O): crt0.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(LIBC) $(CRT0)
