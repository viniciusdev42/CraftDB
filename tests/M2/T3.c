/*
 * Teste de Aceitacao 3
 * ---------------------
 * Verifica que uma alteracao feita em uma pagina, apos essa pagina ser
 * EXPULSA do cache (nao apenas descarregada explicitamente), permanece
 * persistida quando o banco e' reaberto em OUTRO PROCESSO do sistema
 * operacional (memoria estatica nova, cache vazio, nada herdado).
 *
 * Este binario tem dois modos, escolhidos por argv[1], e deve ser
 * executado DUAS VEZES, como dois processos distintos:
 *
 *   ./teste3 escritor   -> processo A: cria o banco, altera uma pagina,
 *                          forca sua expulsao do cache (STEAL) e encerra.
 *   ./teste3 leitor     -> processo B: um NOVO processo, que abre o
 *                          mesmo arquivo de banco do zero e confirma que
 *                          a alteracao feita pelo processo A esta la.
 *
 * Usa NUM_FRAMES=1 para que fixar uma segunda pagina distinta force,
 * de forma inequivoca, a expulsao da primeira (unico jeito de liberar
 * o unico frame disponivel).
 *
 * Compilar:  gcc -std=c11 -Wall -Wextra -o teste3 teste3_persistencia.c
 * Executar:  ./teste3 escritor && ./teste3 leitor
 * Saida de cada execucao: 0 em caso de sucesso, 1 em caso de falha.
 */

#define NUM_FRAMES 1
#define CRAFT_DB_TESTE_SEM_MAIN
#include "M2.c"

#define PAGINA_TESTE   0
#define SLOT_TESTE     0
#define ID_ESPERADO    4242
#define VALOR_ESPERADO 987654321

static void modo_escritor(void) {
    printf("=== Teste 3 [Processo A - escritor] ===\n");
    remove(ARQUIVO_BD); /* comeca de um banco vazio */

    int p0 = aloca_pagina(); /* devera ser a pagina 0, pois o arquivo comeca vazio */
    int p1 = aloca_pagina(); /* segunda pagina, usada apenas para forcar a expulsao de p0 */

    if (p0 != PAGINA_TESTE || p1 < 0) {
        fprintf(stderr, "[FALHOU] Erro de preparacao do teste (p0=%d, p1=%d)\n", p0, p1);
        exit(1);
    }

    unsigned char *buf = fixa(p0);
    if (buf == NULL) {
        fprintf(stderr, "[FALHOU] Nao foi possivel fixar a pagina %d\n", p0);
        exit(1);
    }

    Registro alterado = { .id = ID_ESPERADO, .valor = VALOR_ESPERADO };
    unsigned char bytes[TAMANHO_REGISTRO];
    serializa_registro(&alterado, bytes);
    memcpy(buf + TAMANHO_CABECALHO + SLOT_TESTE * TAMANHO_REGISTRO, bytes, TAMANHO_REGISTRO);

    /* Solta marcando a pagina como SUJA. Politica NO-FORCE: neste momento
       a alteracao ainda NAO foi gravada em disco - existe apenas em memoria. */
    solta(p0, 1);

    /* Com capacidade=1 e p0 ja solta (sem fixacoes), fixar p1 (uma pagina
       diferente) obriga o cache a expulsar p0 para liberar o unico frame. */
    unsigned char *buf2 = fixa(p1);
    if (buf2 == NULL) {
        fprintf(stderr, "[FALHOU] Nao foi possivel fixar a pagina %d\n", p1);
        exit(1);
    }
    solta(p1, 0);

    int p0_ainda_em_cache = (busca_frame(p0) != -1);
    printf("Pagina %d ainda esta em cache apos fixar %d? %s (esperado: nao)\n",
           p0, p1, p0_ainda_em_cache ? "sim" : "nao");

    if (p0_ainda_em_cache) {
        fprintf(stderr, "[FALHOU] A pagina %d nao foi expulsa como esperado.\n", p0);
        exit(1);
    }

    printf("[OK] Alteracao feita, pagina %d expulsa (STEAL grava paginas sujas antes de expulsar).\n", p0);
    printf("Encerrando processo A.\n");
    exit(0);
}

static void modo_leitor(void) {
    printf("=== Teste 3 [Processo B - leitor, processo NOVO e independente] ===\n");
    /* Processo novo: cache_inicializado comeca em 0 e 'cache' comeca zerada -
       nada e' herdado do processo A; qualquer dado aqui vem exclusivamente do disco. */

    unsigned char pg[TAMANHO_PAGINA];
    if (ler_pagina(PAGINA_TESTE, pg, sizeof(pg)) != 0) {
        fprintf(stderr, "[FALHOU] Nao foi possivel ler a pagina %d do disco\n", PAGINA_TESTE);
        exit(1);
    }

    Registro lido;
    desserializa_registro(pg + TAMANHO_CABECALHO + SLOT_TESTE * TAMANHO_REGISTRO, &lido);

    printf("Lido do disco: id=%d, valor=%d (esperado: id=%d, valor=%d)\n",
           lido.id, lido.valor, ID_ESPERADO, VALOR_ESPERADO);

    if (lido.id == ID_ESPERADO && lido.valor == VALOR_ESPERADO) {
        printf("[OK] A alteracao persistiu apos expulsao e reabertura em outro processo.\n");
        exit(0);
    }

    printf("[FALHOU] A alteracao NAO persistiu.\n");
    exit(1);
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Uso: %s [escritor|leitor]\n", argv[0]);
        return 2;
    }
    if (strcmp(argv[1], "escritor") == 0) {
        modo_escritor();
    } else if (strcmp(argv[1], "leitor") == 0) {
        modo_leitor();
    } else {
        fprintf(stderr, "Modo invalido: '%s' (use 'escritor' ou 'leitor')\n", argv[1]);
        return 2;
    }
    return 0;
}
