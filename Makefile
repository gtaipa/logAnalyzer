CC = gcc
CFLAGS = -Wall -Wextra -pthread -I./include

SOCKET_SRC = src/main_sockets.c src/parser.c src/worker_sockets.c src/ipc.c
PIPES_SRC  = src/main_pipes.c src/parser.c src/worker_pipes.c
THREADS_SRC = src/main_threads.c src/parser.c src/worker_threads.c
PRODCONS_SRC = src/main_prodcons.c src/parser.c src/worker_prodcons.c

all: sockets pipes threads prodcons

sockets: logAnalyzer_sockets
pipes: logAnalyzer_pipes
threads: logAnalyzer_threads
prodcons: logAnalyzer_prodcons

logAnalyzer_sockets: $(SOCKET_SRC)
	$(CC) $(CFLAGS) -o $@ $(SOCKET_SRC)

logAnalyzer_pipes: $(PIPES_SRC)
	$(CC) $(CFLAGS) -o $@ $(PIPES_SRC)

logAnalyzer_threads: $(THREADS_SRC)
	$(CC) $(CFLAGS) -o $@ $(THREADS_SRC)

logAnalyzer_prodcons: $(PRODCONS_SRC)
	$(CC) $(CFLAGS) -o $@ $(PRODCONS_SRC)

clean:
	rm -f logAnalyzer_sockets logAnalyzer_pipes logAnalyzer_threads logAnalyzer_prodcons  # isto faz com que o comando make clean excute este codigo apagando todo o conteudo 