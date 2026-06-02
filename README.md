# Log Analyzer — Sistemas Operativos

Sistema de análise paralela de logs em larga escala, implementado em C com
programação concorrente via processos POSIX e threads POSIX.

## Autores

- Pedro José Abreu Rodrigues (2024114929@ufp.edu.pt)
- Guilherme Taipa Nunes(2024118263@ufp.edu.pt)

## Requisitos

- GCC com suporte a C99 ou superior
- POSIX threads (`-lpthread`)
- Sistema operativo Linux/Unix

## Compilação

```bash
make          # compila todos os binários
make basic    # apenas logAnalyzer_basic
make pipes    # apenas logAnalyzer_pipes
make sockets  # apenas logAnalyzer_sockets
make threads  # apenas logAnalyzer_threads
make prodcons # apenas logAnalyzer_prodcons
make clean    # remove binários e ficheiros temporários
```

## Execução

```bash
./logAnalyzer_basic   <diretorio> <num_processos> <modo> [--verbose]
./logAnalyzer_pipes   <diretorio> <num_processos> <modo> [--verbose]
./logAnalyzer_sockets <diretorio> <num_processos> <modo> [--verbose]
./logAnalyzer_threads <diretorio> <num_processos> <modo> [--verbose]
./logAnalyzer_prodcons <diretorio> <num_processos> <modo> [--verbose]
```

### Parâmetros

| Parâmetro | Descrição |
|---|---|
| `<diretorio>` | Pasta com ficheiros `.log` e/ou `.json` a processar |
| `<num_processos>` | Número de workers (processos ou threads) a criar |
| `<modo>` | `security` \| `performance` \| `traffic` \| `full` |
| `--verbose` | Activa saída detalhada durante o processamento |

### Exemplos

```bash
./logAnalyzer_pipes   datasets/apache/ 4 security --verbose
./logAnalyzer_threads datasets/         8 full
./logAnalyzer_sockets datasets/apache/ 4 traffic
./logAnalyzer_prodcons datasets/        4 full
```

## Testes

```bash
make test
```

O alvo `test` executa os cinco binários sobre os datasets de exemplo em
`datasets/` com 2 workers em modo `full` e verifica que todos terminam com
código de saída 0.

## Estrutura do projecto

```
logAnalyzer/
├── src/
│   ├── main_basic.c        # Fase 1B — multi-processo sem IPC
│   ├── main_pipes.c        # Fase 1C/D — pipes anónimos + dashboard
│   ├── main_sockets.c      # Fase 1E — Unix Domain Sockets + dashboard
│   ├── main_threads.c      # Fase 2A/B — threads + dashboard ANSI
│   ├── main_prodcons.c     # Fase 2C — produtor-consumidor
│   ├── worker_pipes.c      # Worker processo (pipes)
│   ├── worker_sockets.c    # Worker processo (sockets)
│   ├── worker_threads.c    # Worker thread (data parallelism)
│   ├── worker_prodcons.c   # Worker produtor e consumidor
│   ├── parser.c            # Pipeline de parsing + métricas
│   ├── log_parser.c        # Parsers Apache / JSON / Syslog / Nginx
│   ├── event_classifier.c  # Classificação de eventos por tipo e severidade
│   ├── ipc.c               # readn, writen, connect_to_server
│   └── posix_io.c          # posix_writef (escrita segura em fd)
├── include/
│   ├── parser.h            # LogEntry, Metrics, LogFormat, enums
│   ├── log_parser.h        # ApacheLogEntry, JSONLogEntry, SyslogEntry, NginxErrorEntry
│   ├── event_classifier.h  # ClassifiedEvent, AnalysisMode, EVENT_*
│   ├── ipc.h               # WorkerConfig, WorkerResult, ProgressUpdate, MSG_*
│   ├── worker.h            # run_worker (sockets)
│   ├── worker_threads.h    # ThreadArgs, run_worker_thread
│   ├── worker_prodcons.h   # BoundedBuffer, ProducerArgs, ConsumerArgs
│   ├── posix_io.h          # posix_writef
│   └── config.h            # MAX_IPS, MAX_ALERTS, constantes globais
├── generators/             # Geradores de datasets sintéticos
├── datasets/               # Amostras de logs para testes
└── Makefile
```

## Arquitectura

### Fase 1 — Processos

| Binário | Mecanismo IPC | Descrição |
|---|---|---|
| `logAnalyzer_basic` | Nenhum | Cada filho escreve `results_<pid>.txt` |
| `logAnalyzer_pipes` | Pipes anónimos | N pipes (um por filho); select(2) no pai |
| `logAnalyzer_sockets` | Unix Domain Sockets | Servidor (pai) + clientes (filhos) |

Todos os workers dividem o espaço de bytes dos ficheiros em fatias iguais
(data parallelism por bytes, não por ficheiros), usando `lseek(2)` para
posicionamento e alinhamento à fronteira de linha seguinte.

### Fase 2 — Threads

| Binário | Padrão | Descrição |
|---|---|---|
| `logAnalyzer_threads` | Data parallelism | N threads com métricas locais; fusão com mutex |
| `logAnalyzer_prodcons` | Produtor-consumidor | Bounded buffer (cap. 30); semáforos POSIX |

### Formatos de log suportados

- **Apache Combined Log Format** — IP, timestamp, método, URL, status, bytes, referer, user-agent
- **JSON Structured Logs** — timestamp ISO 8601, level, service, message, metadata (IP)
- **Syslog RFC 3164** — priority (facility×8+severity), timestamp, hostname, service, PID, message
- **Nginx Error Log** — timestamp, level (emerg…debug), PID, connection ID, message, client IP

### Métricas extraídas

- Total de linhas processadas
- Contagem por nível: DEBUG / INFO / WARN / ERROR / CRITICAL
- Erros HTTP 4xx e 5xx
- Top 10 IPs mais frequentes (ordenados por frequência)
- Alertas de eventos ERROR/CRITICAL

## Geradores de datasets

```bash
make generators
./generate_apache_logs <N_linhas> > datasets/apache/access.log
./generate_json_logs   <N_linhas> > datasets/json_logs/app.json
./generate_syslog      <N_linhas> > datasets/syslog/security.log
./generate_nginx_error <N_linhas> > datasets/nginx/error.log
```
