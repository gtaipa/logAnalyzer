#ifndef WORKER_H
#define WORKER_H

/* =========================================================
 * worker.h  –  Protótipo da função do processo filho
 * ========================================================= */

/**
 * Função principal do processo filho.
 *
 * @param ficheiros  lista completa de ficheiros (partilhada com o pai)
 * @param inicio     índice do primeiro ficheiro a processar
 * @param fim        índice após o último ficheiro a processar
 * @param verbose    1 se --verbose foi passado, 0 caso contrário
 */
void run_worker(char **ficheiros, int inicio, int fim, int verbose);

#endif /* WORKER_H */