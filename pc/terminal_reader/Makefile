# Makefile for the Inky terminal EPUB reader prototype.

CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -I.
LDLIBS  = -lzip

TARGET  = inky_term
SRCS    = main.c \
          epub/epub.c \
          html/html.c \
          layout/layout.c \
          progress/progress.c \
          terminal/terminal.c

OBJS    = $(SRCS:.c=.o)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS) $(LDLIBS)

clean:
	rm -f $(TARGET) $(OBJS)

.PHONY: clean
