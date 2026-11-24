/*
 * Teste de Aceitacao 2
 * ---------------------
 * Cenario: cache com 3 frames (capacidade reduzida via -DNUM_FRAMES=3
 * ou, como aqui, via #define ANTES do #include "cache.c").
 *
 * Verifica que, ao tocar 4 paginas distintas com um cache de apenas
 * 3 frames, a pagina expulsa e' exatamente a MENOS RECENTEMENTE USADA
 * (LRU) - e nao, por exemplo, a primeira que entrou (o que caracterizaria
 * um algoritmo FIFO em vez de LRU). Para isso, a sequencia de acessos
 * "retoca" uma pagina antiga antes de introduzir a 4a pagina, garantindo
 * que o teste distingue de fato LRU de FIFO.
 *
 * Sequencia de acessos (p0, p1, p2 sao as 3 primeiras paginas; p3 a 4a):
 *   fixar/soltar p0                      -> cache: [p0]
 *   fixar/soltar p1                      -> cache: [p0, p1]
 *   fixar/soltar p2                      -> cache: [p0, p1, p2]   (cheio)
 *   fixar/soltar p0 de novo              -> p0 volta a ser a mais recente;
 *                                          p1 passa a ser a MENOS recente
 *   fixar/soltar p3                      -> FALTA, cache cheio -> expulsa
 *                                          a LRU, que deve ser p1
 *
 */

#define NUM_FRAMES 3
#include "../../modules/storage/cache.c"

static int testes_totais = 0;
static int testes_ok = 0;

#define VERIFICA(cond, msg) do { \
    testes_totais++; \
    if (cond) { testes_ok++; printf("  [OK]     %s\n", msg); } \
    else      { printf("  [FALHOU] %s\n", msg); } \
} while (0)

int main(void) {
    remove(ARQUIVO_DB); /* garante um banco de dados limpo antes do teste */

    printf("=== Teste 2: com 3 frames, ao tocar 4 paginas a menos usada e expulsa (LRU) ===\n");

    VERIFICA(cache.capacidade == 3, "capacidade do cache e 3 (definida via NUM_FRAMES)");

    int p0 = alocar_pagina();
    int p1 = alocar_pagina();
    int p2 = alocar_pagina();
    int p3 = alocar_pagina();
    VERIFICA(p0 >= 0 && p1 >= 0 && p2 >= 0 && p3 >= 0, "as 4 paginas de teste foram alocadas com sucesso");

    /* Preenche o cache com as 3 primeiras paginas */
    VERIFICA(fixar(p0) != NULL, "p0 fixada"); soltar(p0, 0);
    VERIFICA(fixar(p1) != NULL, "p1 fixada"); soltar(p1, 0);
    VERIFICA(fixar(p2) != NULL, "p2 fixada"); soltar(p2, 0);

    VERIFICA(cache.uso == 3, "cache esta com uso=3 (cheio, capacidade=3)");
    VERIFICA(buscar_frame(p0) != -1 && buscar_frame(p1) != -1 && buscar_frame(p2) != -1,
              "p0, p1 e p2 estao todas residentes no cache");

    /* Retoca p0: agora p0 e' a mais recentemente usada; p1 passa a ser a LRU */
    VERIFICA(fixar(p0) != NULL, "p0 refixada (retocada)"); soltar(p0, 0);

    /* Introduz a 4a pagina distinta: forca uma expulsao no cache cheio */
    unsigned char *ref_p3 = fixar(p3);
    VERIFICA(ref_p3 != NULL, "p3 fixada com sucesso (miss que forca expulsao)");
    soltar(p3, 0);

    VERIFICA(cache.uso == 3, "apos a expulsao, o cache continua com uso=3 (capacidade respeitada)");

    /* Verificacao central do teste: p1 (a LRU) foi expulsa; as demais permanecem */
    VERIFICA(buscar_frame(p1) == -1, "p1 (a MENOS recentemente usada) foi expulsa do cache");
    VERIFICA(buscar_frame(p0) != -1, "p0 (retocada por ultimo) permanece no cache");
    VERIFICA(buscar_frame(p2) != -1, "p2 permanece no cache");
    VERIFICA(buscar_frame(p3) != -1, "p3 (recem-fixada) esta no cache");

    VERIFICA(cache.faltas == 4, "4 faltas no total (p0, p1, p2 e p3, cada uma na primeira fixacao)");
    VERIFICA(cache.acertos == 1, "1 acerto no total (a refixacao de p0)");

    printf("Resultado: %d/%d verificacoes passaram.\n", testes_ok, testes_totais);
    return (testes_ok == testes_totais) ? 0 : 1;
}
