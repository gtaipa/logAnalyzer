 CC = gcc
CFLAGS = -Wall -Wextra -I./include
SRC = src/main.c src/parser.c src/worker.c src/ipc.c
TARGET = logAnalyzer

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

clean:
	rm -f $(TARGET)
