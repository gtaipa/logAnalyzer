#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

int main(int argc, char *argv[]) {

    if (argc < 4) {
        printf("O comando introduzido não conta com todos os argumentos obrigatorios.\n");
        printf("Exemplo: %s /var/log/ 4 security --verbose\n", argv[0]);
        exit(1);
    }

    char *diretorio = argv[1];
    int num_processos = atoi(argv[2]);
    char *modo = argv[3];

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

    /* Abrir a pasta */
    DIR *dir = opendir(diretorio);
    if (dir == NULL) {
        perror("opendir");
        exit(1);
    }

    /* Percorrer os ficheiros e filtrar só os .log */
    struct dirent *entrada;
    while ((entrada = readdir(dir)) != NULL) {
        char *nome = entrada->d_name;
        int len = strlen(nome);
        if (len > 4 && strcmp(nome + len - 4, ".log") == 0) {
            printf("Encontrei ficheiro log: %s\n", nome);
        }
    }

    /* Fechar a pasta depois de percorrer tudo */
    closedir(dir);

    return 0;
}
