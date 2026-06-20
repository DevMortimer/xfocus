CC = gcc
CFLAGS = -Wall -Wextra -O2
LDFLAGS = -lX11 -lXext -lXinerama -lGL -lm

TARGET = xfocus
SRC = main.c
OBJ = $(SRC:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJ) Makefile
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

%.o: %.c Makefile
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)

run: $(TARGET)
	./$(TARGET)

.PHONY: all clean run
