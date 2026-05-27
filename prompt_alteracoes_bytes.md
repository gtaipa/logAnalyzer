# Prompt para IA — Alteração de divisão por linhas para divisão por bytes

## Contexto

Temos um projeto em C de análise paralela de logs (logAnalyzer).
O sistema cria N processos filho (workers), cada um responsável por processar
uma parte dos ficheiros de log.

## Problema actual

Nos ficheiros `src/main_pipes.c` e `src/main_sockets.c` existe uma função
chamada `contar_todas_linhas()` que abre todos os ficheiros e lê o seu
conteúdo inteiro apenas para contar quantas linhas existem.

Depois, cada worker recebe um intervalo de linhas (ex: worker 0 fica com as
linhas 0 a 1000, worker 1 fica com 1001 a 2000, etc.) e volta a abrir os
ficheiros desde o início, lendo e ignorando linhas até chegar à sua parte.

Resultado: cada byte dos ficheiros é lido N+1 vezes (1 vez para contar + 1 vez
por cada worker). Isto é completamente desnecessário.

## Solução pretendida

Dividir o trabalho por **bytes** em vez de linhas, usando `stat()` e `lseek()`:

1. Usar `stat()` em cada ficheiro para obter o seu tamanho em bytes — esta
   chamada ao sistema não lê o conteúdo do ficheiro, apenas consulta a
   sua metadata.

2. Somar os bytes de todos os ficheiros e dividir igualmente pelos N workers.

3. Cada worker recebe `byte_inicio` e `byte_fim` (os seus limites em bytes).

4. O worker usa `lseek()` para saltar directamente para o byte certo dentro
   do ficheiro correcto, sem ter de ler o que vem antes.

5. Como o offset pode cair a meio de uma linha, o worker avança até ao
   próximo `\n` para garantir que só processa linhas completas.

## Ficheiros a alterar

### `include/ipc.h`

Na struct `WorkerConfig`, substituir os campos de linhas por campos de bytes:

```c
// ANTES:
typedef struct {
    long linha_inicio;
    long linha_fim;
    long total_linhas_globais;
    int worker_index;
} WorkerConfig;

// DEPOIS:
typedef struct {
    off_t byte_inicio;
    off_t byte_fim;
    off_t total_bytes_globais;
    int worker_index;
} WorkerConfig;
```

Na struct `ProgressUpdate`, renomear os campos:

```c
// ANTES:
typedef struct {
    pid_t pid;
    int   worker_index;
    long  lines_done;
    long  lines_total;
} ProgressUpdate;

// DEPOIS:
typedef struct {
    pid_t pid;
    int   worker_index;
    long  bytes_done;
    long  bytes_total;
} ProgressUpdate;
```

Adicionar `#include <sys/types.h>` no topo (necessário para `off_t`).

Actualizar a declaração de `run_worker_pipe` para usar `off_t` nos dois
últimos parâmetros em vez de `long`.

---

### `src/main_pipes.c` e `src/main_sockets.c`

Adicionar `#include <sys/stat.h>` no topo.

Substituir a função `contar_todas_linhas()` por uma função simples que usa
`stat()`:

```c
static off_t obter_bytes_totais(char **ficheiros, int total_ficheiros) {
    off_t total = 0;
    struct stat st;
    for (int i = 0; i < total_ficheiros; i++) {
        if (stat(ficheiros[i], &st) == 0)
            total += st.st_size;
    }
    return total;
}
```

No `main()`, substituir o bloco que chama `contar_todas_linhas` e calcula
`linhas_por_worker` pelo seguinte:

```c
off_t total_bytes     = obter_bytes_totais(ficheiros, total_ficheiros);
off_t bytes_por_worker = total_bytes / num_processos;

for (int i = 0; i < num_processos; i++) {
    configs[i].worker_index        = i;
    configs[i].byte_inicio         = (off_t)i * bytes_por_worker;
    configs[i].byte_fim            = (i == num_processos - 1)
                                     ? total_bytes
                                     : configs[i].byte_inicio + bytes_por_worker;
    configs[i].total_bytes_globais = total_bytes;
}
```

No dashboard, mudar `progressos[i].lines_done` e `lines_total` para
`bytes_done` e `bytes_total`. Mudar também o texto "linhas" para "bytes"
na string do printf.

Onde o resultado é marcado como 100%, mudar:
```c
// ANTES:
progressos[i].lines_done = progressos[i].lines_total;
// DEPOIS:
progressos[i].bytes_done = progressos[i].bytes_total;
```

---

### `src/worker_pipes.c` e `src/worker_sockets.c`

Adicionar `#include <sys/stat.h>` e `#include <sys/types.h>`.

Mudar a assinatura da função principal do worker de `long linha_inicio, long linha_fim`
para `off_t byte_inicio, off_t byte_fim`.

Substituir toda a lógica de "contar linhas globais e saltar as que não são minhas"
pela seguinte lógica por bytes:

```
global_offset = 0   (bytes acumulados desde o início da lista de ficheiros)

para cada ficheiro[i]:
    obter tamanho com stat()

    se (global_offset + tamanho <= byte_inicio):
        este ficheiro é completamente antes da nossa fatia → ignorar
        global_offset += tamanho
        continuar

    se (global_offset >= byte_fim):
        este ficheiro é completamente depois da nossa fatia → parar

    local_start = byte_inicio - global_offset  (ou 0 se o ficheiro começa dentro da fatia)
    local_end   = byte_fim - global_offset      (ou tamanho_ficheiro se a fatia acaba depois)

    abrir o ficheiro com open()
    lseek(fd, local_start, SEEK_SET)   ← saltar directamente para o byte certo

    se local_start > 0:
        ler byte a byte até encontrar '\n'   ← descartar linha parcial do inicio

    ler e processar linhas normalmente até file_pos >= local_end
    quando file_pos >= local_end, terminar a linha actual antes de parar

    fechar ficheiro
    global_offset += tamanho
```

Para o progresso, calcular:
```c
off_t bytes_done = global_offset + file_pos - byte_inicio;
if (bytes_done > quota) bytes_done = quota;
enviar_progresso(..., (long)bytes_done, (long)quota);
```

A função `enviar_progresso` passa a usar `bytes_done` e `bytes_total` nos campos
do `ProgressUpdate` em vez de `lines_done` e `lines_total`.

---

## Regras para a implementação

- Código simples e directo, sem abstrações desnecessárias
- Não usar `fopen`, `fread`, `fwrite` — apenas `open`, `read`, `write`, `lseek`, `stat`
- Verificar sempre o retorno das chamadas ao sistema com `perror()` em caso de erro
- Os ficheiros de threads (`main_threads.c`, `worker_threads.c`) e de
  produtor-consumidor (`main_prodcons.c`, `worker_prodcons.c`) **não devem ser
  alterados** — usam estruturas de dados próprias e independentes
- Manter os comentários existentes e o estilo de código do projecto
