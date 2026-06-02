CC = gcc
CFLAGS = -Wall -Wextra -pthread -I./include

# ─── Fontes por binário ───────────────────────────────────────────────────────
BASIC_SRC    = src/main_basic.c src/parser.c src/log_parser.c src/event_classifier.c src/posix_io.c
SOCKET_SRC   = src/main_sockets.c src/parser.c src/log_parser.c src/event_classifier.c src/worker_sockets.c src/ipc.c src/posix_io.c
PIPES_SRC    = src/main_pipes.c src/parser.c src/log_parser.c src/event_classifier.c src/worker_pipes.c src/ipc.c src/posix_io.c
THREADS_SRC  = src/main_threads.c src/parser.c src/log_parser.c src/event_classifier.c src/worker_threads.c src/posix_io.c
PRODCONS_SRC = src/main_prodcons.c src/parser.c src/log_parser.c src/event_classifier.c src/worker_prodcons.c src/posix_io.c

# ─── Alvos principais ─────────────────────────────────────────────────────────
all: basic sockets pipes threads prodcons

basic:   logAnalyzer_basic
sockets: logAnalyzer_sockets
pipes:   logAnalyzer_pipes
threads: logAnalyzer_threads
prodcons: logAnalyzer_prodcons

# ─── Regras de compilação ─────────────────────────────────────────────────────
# Req. B — multi-processo básico sem IPC (cada filho escreve results_<pid>.txt)
logAnalyzer_basic: $(BASIC_SRC)
	$(CC) $(CFLAGS) -o $@ $(BASIC_SRC)

logAnalyzer_sockets: $(SOCKET_SRC)
	$(CC) $(CFLAGS) -o $@ $(SOCKET_SRC)

logAnalyzer_pipes: $(PIPES_SRC)
	$(CC) $(CFLAGS) -o $@ $(PIPES_SRC)

logAnalyzer_threads: $(THREADS_SRC)
	$(CC) $(CFLAGS) -o $@ $(THREADS_SRC)

logAnalyzer_prodcons: $(PRODCONS_SRC)
	$(CC) $(CFLAGS) -o $@ $(PRODCONS_SRC)

generators: generate_apache_logs generate_json_logs generate_syslog generate_nginx_error

generate_apache_logs: generators/generate_apache_logs.c
	$(CC) -Wall -Wextra -o $@ $<

generate_json_logs: generators/generate_json_logs.c
	$(CC) -Wall -Wextra -o $@ $<

generate_syslog: generators/generate_syslog.c
	$(CC) -Wall -Wextra -o $@ $<

generate_nginx_error: generators/generate_nginx_error.c
	$(CC) -Wall -Wextra -o $@ $<

test: all
	@echo "=== Teste logAnalyzer_basic ==="
	./logAnalyzer_basic   datasets/ 2 full && echo "OK basic"
	@echo "=== Teste logAnalyzer_pipes ==="
	./logAnalyzer_pipes   datasets/ 2 full && echo "OK pipes"
	@echo "=== Teste logAnalyzer_sockets ==="
	./logAnalyzer_sockets datasets/ 2 full && echo "OK sockets"
	@echo "=== Teste logAnalyzer_threads ==="
	./logAnalyzer_threads datasets/ 2 full && echo "OK threads"
	@echo "=== Teste logAnalyzer_prodcons ==="
	./logAnalyzer_prodcons datasets/ 2 full && echo "OK prodcons"
	@echo "=== Todos os testes passaram ==="

clean:
	rm -f logAnalyzer_basic logAnalyzer_sockets logAnalyzer_pipes logAnalyzer_threads logAnalyzer_prodcons
	rm -f results_*.txt meu_relatorio.txt
	rm -f generate_apache_logs generate_json_logs generate_syslog generate_nginx_error
