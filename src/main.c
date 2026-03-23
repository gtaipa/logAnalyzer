#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>    /* fork(), getpid() */
#include <sys/wait.h>  /* wait() */

int main(int argc, char *argv[]) {

    /* Verificar se foram passados os argumentos obrigatórios */
    if (argc < 4) {
        printf("O comando introduzido não conta com todos os argumentos obrigatorios.\n");
        printf("Exemplo: %s /var/log/ 4 security --verbose\n", argv[0]);
        exit(1);
    }

    /* Guardar os argumentos em variáveis com nomes claros */
    char *diretorio   = argv[1];
    int num_processos = atoi(argv[2]);
    char *modo        = argv[3];

    /* Verificar se o --verbose foi passado */
    int verbose = 0;
    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--verbose") == 0) {
            verbose = 1;
        }
    }

    printf("Diretorio: %s\n", diretorio);
    printf("Processos: %d\n", num_processos);
    printf("Modo: %s\n", modo);
    printf("Verbose: %s\n", verbose ? "sim" : "nao");

    /* ---- LISTA DINÂMICA DE FICHEIROS ----
     * Em vez de um array fixo, começamos com espaço para 10 ficheiros.
     * Se precisarmos de mais, o realloc duplica o espaço automaticamente.
     * É como uma mochila que cresce quando fica cheia. */
    int capacidade = 10;
    int total_ficheiros = 0;
    char **ficheiros = malloc(capacidade * sizeof(char *));
    if (ficheiros == NULL) {
        perror("malloc");
        exit(1);
    }

    /* ---- ABRIR A PASTA E ENCONTRAR OS FICHEIROS .log ---- */
    DIR *dir = opendir(diretorio);
    if (dir == NULL) {
        perror("opendir");
        exit(1);
    }

    struct dirent *entrada;
    while ((entrada = readdir(dir)) != NULL) {

        char *nome = entrada->d_name;
        int len = strlen(nome);

        /* Ignorar tudo o que não terminar em .log */
        if (len > 4 && strcmp(nome + len - 4, ".log") == 0) {

            /* Se a lista estiver cheia, duplicamos o espaço.
             * Por exemplo: 10 -> 20 -> 40 -> 80... */
            if (total_ficheiros == capacidade) {
                capacidade *= 2;
                ficheiros = realloc(ficheiros, capacidade * sizeof(char *));
                if (ficheiros == NULL) {
                    perror("realloc");
                    exit(1);
                }
            }

            /* Construir o caminho completo: "datasets/" + "teste.log"
             * = "datasets/teste.log" */
            char caminho[512];
            snprintf(caminho, sizeof(caminho), "%s/%s", diretorio, nome);

            /* strdup copia a string para memória própria e guarda na lista */
            ficheiros[total_ficheiros] = strdup(caminho);
            total_ficheiros++;

            if (verbose) printf("Encontrei: %s\n", ficheiros[total_ficheiros - 1]);
        }
    }

    /* Fechar a pasta — já não precisamos dela */
    closedir(dir);

    printf("Total de ficheiros .log encontrados: %d\n", total_ficheiros);
/* Confirmar que a lista foi preenchida corretamente */
printf("\n--- Conteudo da lista ---\n");
for (int i = 0; i < total_ficheiros; i++) {
    printf("ficheiros[%d] = %s\n", i, ficheiros[i]);
}

/* ---- CRIAR OS PROCESSOS FILHO COM FORK() ----
 * Vamos criar N processos filho, um por cada processo pedido.
 * Cada filho vai processar uma parte dos ficheiros da lista. */

/* Calcular quantos ficheiros cada filho vai processar.
 * Exemplo: 6 ficheiros e 2 processos = 3 ficheiros por processo */
int ficheiros_por_processo = total_ficheiros / num_processos;

for (int i = 0; i < num_processos; i++) {

    pid_t pid = fork(); /* o processo clona-se aqui! */

    if (pid < 0) {
        /* fork() devolveu negativo = algo correu mal */
        perror("fork");
        exit(1);

    } else if (pid == 0) {
        /* ---- ESTAMOS DENTRO DO FILHO ----
         * Cada filho calcula quais ficheiros lhe pertencem
         * com base no seu número (i) */

        int inicio = i * ficheiros_por_processo;
        int fim = (i == num_processos - 1) ? total_ficheiros : inicio + ficheiros_por_processo;

        printf("[Filho %d] Vou processar %d ficheiros\n", getpid(), fim - inicio);

        for (int j = inicio; j < fim; j++) {
            printf("[Filho %d] A processar: %s\n", getpid(), ficheiros[j]);
        }

        exit(0); /* filho termina aqui — MUITO IMPORTANTE! */
    }
    /* o pai continua o loop e cria o próximo filho */
}

/* ---- PAI ESPERA QUE TODOS OS FILHOS TERMINEM ----
 * O wait() bloqueia o pai até um filho terminar.
 * Como temos N filhos, chamamos wait() N vezes. */
for (int i = 0; i < num_processos; i++) {
    wait(NULL);
}

printf("\n[Pai] Todos os workers terminaram!\n");


    return 0;
}