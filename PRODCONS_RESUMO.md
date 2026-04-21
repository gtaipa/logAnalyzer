# 🏗️ Modelo Produtor-Consumidor - Resumo da Implementação

## ✅ Ficheiros Criados

### 1. **include/worker_prodcons.h**

O cabeçalho que define toda a arquitetura:

```c
/* 📦 O Armazém (Bounded Buffer) - A passadeira rolante */
typedef struct {
    LogEntry buffer[BUFFER_SIZE];           // Array circular (100 items)
    int in;                                  // Índice para inserção
    int out;                                 // Índice para remoção
    int count;                               // Items no buffer neste momento

    pthread_mutex_t mutex;                   // Cadeado exclusivo
    pthread_cond_t cond_not_full;           // Sinal: buffer não cheio
    pthread_cond_t cond_not_empty;          // Sinal: buffer não vazio
} BoundedBuffer;
```

**Variável Global**: `int produtores_ativos` (alarme de fim de turno)

---

### 2. **src/worker_prodcons.c**

Implementação das threads produtoras e consumidoras:

#### 🧑‍🌾 **Produtor (run_producer)**

```
1. Lê ficheiros de log linha a linha
2. Parseia cada linha com parse_line()
3. TRANCAR o mutex
4. ESPERAR na cond_not_full se buffer cheio
5. INSERIR no buffer (índice 'in')
6. SINALIZAR cond_not_empty
7. DESTRANCAR
```

#### 🕵️‍♂️ **Consumidor (run_consumer)**

```
1. Entra num loop infinito
2. TRANCAR o mutex
3. ESPERAR na cond_not_empty se buffer vazio (enquanto produtores_ativos)
4. Se vazio E produtores inactivos → SAIR
5. RETIRAR do buffer (índice 'out')
6. SINALIZAR cond_not_full
7. DESTRANCAR
8. ATUALIZAR métricas (protegido por outro mutex)
```

---

### 3. **src/main_prodcons.c**

O "Diretor" que orquestra tudo:

```
[1] Descobrir ficheiros .log
[2] Inicializar BoundedBuffer e mutexes
[3] CRIAR threads produtoras
[4] CRIAR threads consumidoras
[5] pthread_join PRODUTORES
    ↓
    Produtores terminaram = fim de ficheiros
[6] ALARME: produtores_ativos = 0
[7] pthread_cond_broadcast → acordar TODOS os consumidores
[8] pthread_join CONSUMIDORES
[9] Imprimir relatório final
```

---

## 🔄 Fluxo de Sincronização

```
PRODUTOR                          BUFFER                    CONSUMIDOR
   │                                │                            │
   ├─ Lock mutex ─────────────────→│                            │
   │                                │                            │
   ├─ Wait if full ────────────────→│◄─── Wait if empty ────────┤
   │                                │                            │
   ├─ Inserir (in) ────────────────→│                            │
   │  count++                        │                            │
   │                                │                            │
   ├─ Signal NOT_EMPTY ────────────→│─── Acordar ───────────────┤
   │                                │                            │
   ├─ Unlock ──────────────────────→│                            │
   │                                │    ├─ Lock mutex ─────────┤
   │                                │    │                       │
   │                                │    ├─ Retirar (out)        │
   │                                │    │  count--              │
   │                                │    │                       │
   │                                │    ├─ Signal NOT_FULL ────→│
   │                                │    │  (acordar produtores)  │
   │                                │    │                       │
   │                                │    └─ Unlock              │
   │                                │
   └─ Se não há mais ficheiros ────→│
      produtores_ativos = 0
      broadcast para consumidores
                                     └─ Consumidores veem:
                                        buffer vazio + inactivos
                                        → SAIR
```

---

## 📊 Resultado de Teste

```
[MAIN] Descobertos 1 ficheiros de log.
[MAIN] A usar 2 produtores e 3 consumidores.
[MAIN] A lançar 2 threads produtoras...
[MAIN] A lançar 3 threads consumidoras...
...
=== RELATORIO FINAL PRODUTOR-CONSUMIDOR (full) ===
Total de linhas : 6

--- ALERTAS DE SEGURANCA ---
DEBUG           : 0
INFO            : 3
WARNINGS        : 2
ERRORS          : 1
CRITICAL        : 0

--- ESTATISTICAS DE TRAFEGO ---
HTTP 4xx        : 2
HTTP 5xx        : 1

--- TOP IPs ---
192.168.1.1 : 3 accesos
192.168.1.2 : 2 accesos
192.168.1.3 : 1 accesos
```

---

## 🚀 Como Usar

### Compilar:

```bash
cd logAnalyzer
make prodcons
```

### Executar:

```bash
# Modo simples
./logAnalyzer_prodcons <diretorio> <num_produtores> <num_consumidores> <modo>

# Exemplo com 2 produtores, 3 consumidores
./logAnalyzer_prodcons ./logs_apache 2 3 full

# Com modo verbose
./logAnalyzer_prodcons ./logs_apache 2 3 full --verbose

# Com ficheiro de output
./logAnalyzer_prodcons ./logs_apache 2 3 full --output=relatorio.txt
```

### Modos suportados:

- `security` - Alertas de segurança
- `traffic` - Estatísticas de tráfego
- `full` - Tudo

---

## 🔑 Pontos-Chave da Implementação

✅ **Buffer Circular**: Usa índices `in` e `out` para reutilizar espaço  
✅ **Mutex + Condition Variables**: Sincronização eficiente e segura  
✅ **Alarme de Fim**: Variável global `produtores_ativos` coordena término  
✅ **Broadcast**: `pthread_cond_broadcast()` acorda TODOS os consumidores de uma vez  
✅ **Métricas Protegidas**: Outro mutex para atualizar métricas globais  
✅ **Sem Deadlocks**: Ordem consistente de aquisição de locks  
✅ **Sem Memory Leaks**: Liberta todos os recursos adequadamente

---

## 📝 Notas Importantes

1. **O código anterior não foi apagado**: Todos os ficheiros antigos (pipes, sockets, threads) continuam intactos
2. **Makefile atualizado**: Adicionada target `prodcons` e `make all` agora compila tudo
3. **Novo executável**: `logAnalyzer_prodcons` criado com sucesso
4. **Buffer de 100 items**: Pode ser ajustado em `#define BUFFER_SIZE 100`

---

**Implementação Completa! 🎉**
