/**
 * @file ipc.c
 * @brief Funções auxiliares de IPC (Inter-Process Communication)
 * @author Implementação do Analisador de Logs
 * @version 1.0
 * @date 2026
 * 
 * @details
 * Fornece primitivas de baixo nível para comunicação entre processos via
 * Unix Domain Sockets e funções de leitura/escrita robustas para pipes e sockets.
 */

#include "ipc.h" // Incluir header com declarações de funções IPC
#include <unistd.h> // Incluir header com funções POSIX
#include <string.h> // Incluir header com funções de string
#include <stdio.h> // Incluir header com funções de I/O
#include <sys/socket.h> // Incluir header com funções de socket
#include <sys/un.h> // Incluir header com estruturas Unix domain socket
#include <errno.h> // Incluir header com variável de erro

/**
 * @brief Conecta a um servidor Unix Domain Socket com retry automático
 * 
 * @details
 * Função para cliente que tenta estabelecer ligação com o servidor (pai).
 * Se o servidor ainda não está pronto, tenta múltiplas vezes com delay.
 * 
 * @return File descriptor do socket ligado (>= 0) em sucesso, -1 em erro
 */
int connect_to_server(void) { // Função para conectar ao servidor

    int fd = socket(AF_UNIX, SOCK_STREAM, 0); // Criar socket Unix domain
    if (fd < 0) { // Se erro ao criar socket
        perror("socket"); // Imprimir erro
        return -1; // Retornar erro
    }

    struct sockaddr_un addr; // Declarar estrutura de endereço
    memset(&addr, 0, sizeof(addr)); // Limpar estrutura
    addr.sun_family = AF_UNIX; // Atribuir família Unix
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1); // Copiar caminho

    int tentativas = 10; // Inicializar contador de tentativas
    while (tentativas-- > 0) { // Ciclo até 10 tentativas
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) { // Se conexão ok
            return fd; // Retornar file descriptor
        }
        usleep(50000); // Esperar 50ms antes de tentar novamente
    }

    perror("connect"); // Imprimir erro
    close(fd); // Fechar socket
    return -1; // Retornar erro
}

/**
 * @brief Lê exatamente nbytes de um file descriptor com retry automático
 * 
 * @details
 * Em pipes e sockets, read() pode devolver menos bytes que pedidos.
 * Esta função repete read() até completar nbytes ou encontrar EOF/erro.
 * Trata EINTR para recuperar de sinais interrompendo a leitura.
 * 
 * @param fd Descritor de ficheiro a ler
 * @param ptr Ponteiro para buffer de destino
 * @param nbytes Número de bytes a ler
 * @return Número de bytes efetivamente lidos (pode ser < nbytes em EOF), -1 em erro
 */
ssize_t readn(int fd, void *ptr, size_t nbytes) { // Função para ler n bytes
    size_t nleft = nbytes; // Inicializar bytes em falta
    char *buf_ptr = ptr; // Inicializar ponteiro do buffer

    while (nleft > 0) { // Ciclo enquanto há bytes por ler
        ssize_t nread = read(fd, buf_ptr, nleft); // Ler do file descriptor

        if (nread == -1 && errno == EINTR) { // Se interrompido por sinal
            continue; // Continuar o ciclo
        }

        if (nread == -1) { // Se erro real
            return -1; // Retornar erro
        }

        if (nread == 0) { // Se EOF atingido
            break; // Sair do ciclo
        }

        nleft -= (size_t)nread; // Decrementar bytes em falta
        buf_ptr += nread; // Avançar ponteiro do buffer
    }

    return (ssize_t)(nbytes - nleft); // Retornar total lido
}

/**
 * @brief Escreve exatamente nbytes num file descriptor com retry automático
 * 
 * @details
 * Em pipes e sockets, write() pode aceitar menos bytes que pedidos.
 * Esta função repete write() até enviar tudo ou encontrar erro real.
 * Trata EINTR para recuperar de sinais interrompendo a escrita.
 * 
 * @param fd Descritor de ficheiro para escrever
 * @param ptr Ponteiro para buffer de origem
 * @param nbytes Número de bytes a escrever
 * @return Número de bytes escritos (= nbytes se sucesso), -1 em erro
 */
ssize_t writen(int fd, void *ptr, size_t nbytes) { // Função para escrever n bytes
    size_t nleft = nbytes; // Inicializar bytes por escrever
    char *buf_ptr = ptr; // Inicializar ponteiro do buffer

    while (nleft > 0) { // Ciclo enquanto há bytes por escrever
        ssize_t nwritten = write(fd, buf_ptr, nleft); // Escrever no file descriptor

        if (nwritten == -1 && errno == EINTR) { // Se interrompido por sinal
            continue; // Continuar o ciclo
        }

        if (nwritten == -1) { // Se erro real
            return -1; // Retornar erro
        }

        if (nwritten == 0) { // Se sem progresso
            errno = EPIPE; // Atribuir erro de pipe
            return -1; // Retornar erro
        }

        nleft -= (size_t)nwritten; // Decrementar bytes por escrever
        buf_ptr += nwritten; // Avançar ponteiro do buffer
    }

    return (ssize_t)nbytes; // Retornar total escrito
}
