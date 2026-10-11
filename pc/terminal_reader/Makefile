# Simple Makefile for the Inky terminal EPUB reader prototype.

CC      = gcc
CFLAGS  = -Wall -Wextra -O2
LDLIBS  = -lzip

TARGET  = inky_term
SRC     = main.c

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) $(LDLIBS)

clean:
	rm -f $(TARGET)

.PHONY: clean
