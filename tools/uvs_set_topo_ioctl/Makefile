CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=c99

TARGET := uvs_set_topo_ioctl
SRCS := main.c

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $(SRCS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
