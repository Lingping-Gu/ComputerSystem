CC = gcc
CFLAGS = -pthread -Wall -Wextra -O2
TARGET = project2
SRC = sample7.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

clean:
	rm -f $(TARGET)
