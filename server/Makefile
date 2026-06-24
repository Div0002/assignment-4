CC := $(CROSS_COMPILE)gcc

CFLAGS ?= -Wall -Wextra -Werror -std=c11
LDFLAGS ?=

TARGET := aesdsocket
SOURCE := aesdsocket.c

.PHONY: all default clean

default: all

all: $(TARGET)

$(TARGET): $(SOURCE)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

clean:
	$(RM) $(TARGET)
