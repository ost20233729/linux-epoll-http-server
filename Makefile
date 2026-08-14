CC := gcc
CFLAGS := -std=gnu11 -O2 \
	-Wall -Wextra -Wpedantic -Wshadow -Wconversion \
	-Iinclude
LDFLAGS :=
TARGET := minihttpd
SOURCES := $(wildcard src/*.c)
OBJECTS := $(SOURCES:.c=.o)

.PHONY: all clean debug test

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

debug: CFLAGS := -std=gnu11 -O0 -g3 \
	-Wall -Wextra -Wpedantic -Wshadow -Wconversion \
	-fsanitize=address,undefined \
	-Iinclude
debug: LDFLAGS := -fsanitize=address,undefined
debug: clean all

test: all
	bash tests/test.sh

clean:
	rm -f $(OBJECTS) $(TARGET)
