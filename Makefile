CC = gcc
CFLAGS = -Wall -Wextra -I./include

SOCKET_SRC = src/main_sockets.c src/parser.c src/worker_sockets.c src/ipc.c
PIPES_SRC  = src/main_pipes.c src/parser.c src/worker_pipes.c

all: sockets pipes

sockets: logAnalyzer_sockets
pipes: logAnalyzer_pipes

logAnalyzer_sockets: $(SOCKET_SRC)
	$(CC) $(CFLAGS) -o $@ $(SOCKET_SRC)

logAnalyzer_pipes: $(PIPES_SRC)
	$(CC) $(CFLAGS) -o $@ $(PIPES_SRC)

clean:
	rm -f logAnalyzer_sockets logAnalyzer_pipes
