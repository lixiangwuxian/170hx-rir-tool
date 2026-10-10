CC = gcc
CFLAGS = -O2 -Wall -Wextra
TARGET = ga100_rir

all: $(TARGET)

$(TARGET): ga100_rir.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)

.PHONY: all clean
