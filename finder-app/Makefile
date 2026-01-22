# Compiler and flags
CC = $(CROSS_COMPILE)gcc
CFLAGS = -Wall -Werror
TARGET = writer
SRC = writer.c
OBJ = $(SRC:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJ)

clean:
	rm -f $(TARGET) *.o

.PHONY: all clean
