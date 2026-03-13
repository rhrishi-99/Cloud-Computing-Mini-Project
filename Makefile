# Makefile for Mini-UnionFS
# Requires: libfuse3-dev  (sudo apt-get install libfuse3-dev)

CC      = gcc
CFLAGS  = -Wall -Wextra -g -O2 $(shell pkg-config fuse3 --cflags)
LDFLAGS = $(shell pkg-config fuse3 --libs)
TARGET  = mini_unionfs
SRC     = mini_unionfs.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(TARGET)
