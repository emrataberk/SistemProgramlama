CC=gcc
CFLAGS=-Wall -Wextra -std=c11 -pedantic -O2
TARGET=tarsau
SRC=src/tarsau.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

clean:
	rm -f $(TARGET) *.sau
	rm -rf out test_out

.PHONY: all clean
