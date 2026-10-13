# Makefile for the Inky SDL2 reader prototype.

CC      = gcc
CFLAGS  = -Wall -Wextra -O2 `sdl2-config --cflags` -I../../shared
LDLIBS  = `sdl2-config --libs` -lSDL2_ttf -lzip

TARGET  = inky_sdl
SRCS    = main.c \
          ../../shared/books_dir/books_dir.c \
          ../../shared/book_list/book_list.c \
          ../../shared/epub/epub.c \
          ../../shared/html/html.c \
          ../../shared/layout/layout.c \
          ../../shared/progress/progress.c

OBJS    = $(SRCS:.c=.o)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS) $(LDLIBS)

clean:
	rm -f $(TARGET) $(OBJS)

.PHONY: clean
