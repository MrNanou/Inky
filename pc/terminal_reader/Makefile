# Makefile for the Inky terminal EPUB reader prototype.
# Builds from the shared core engine plus terminal-specific I/O.

CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -MMD -I. -I../../shared
LDLIBS  = -lzip

TARGET  = inky_term
SRCS    = main.c \
          terminal/terminal.c \
          ../../shared/books_dir/books_dir.c \
          ../../shared/book_list/book_list.c \
          ../../shared/epub/epub.c \
          ../../shared/html/html.c \
          ../../shared/layout/layout.c \
          ../../shared/progress/progress.c \
          ../../shared/reader/reader.c

OBJS    = $(SRCS:.c=.o)
DEPS    = $(SRCS:.c=.d)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS) $(LDLIBS)

clean:
	rm -f $(TARGET) $(OBJS) $(DEPS)

.PHONY: clean

-include $(DEPS)
