CC ?= $(CROSS_COMPILE)gcc
CFLAGS=-Wall -Wextra -Werror

all: writer

writer: writer.c
	$(CC) $(CFLAGS) writer.c -o writer

clean:
	rm -f writer *.o
