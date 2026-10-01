/*
 * Teste de Aceitacao 1
 * ---------------------
 * Cenario: cache com 4 frames (capacidade padrao de M2.c).
 *
 * Verifica que MULTIPLAS leituras (fixa) da MESMA pagina resultam em uma
 * UNICA leitura fisica em disco: a primeira fixa() deve ser falta (miss) e
 * disparar exatamente 1 leitura em disco; todas as fixacoes seguintes da
 * mesma pagina devem ser acertos (hits), sem tocar o disco novamente, e
 * devem devolver sempre a MESMA referencia de buffer.
 *
 * Este teste inclui M2.c diretamente (em vez de linkar como biblioteca)
 * para ter acesso a estrutura interna 'cache' e a 'busca_frame()', o que
 * permite verificacoes mais rigorosas do que apenas observar o retorno
 * de fixa(). CRAFT_DB_TESTE_SEM_MAIN suprime o main() de demonstracao
 * de M2.c para que este arquivo possa definir o seu proprio.
 *
 * Compilar:  gcc -std=c11 -Wall -Wextra -o teste1 teste1_leitura_unica.c
 * Executar:  ./teste1
 * Saida: 0 se todas as verificacoes passarem, 1 caso alguma falhe.
 */

#define CRAFT_DB_TESTE_SEM_MAIN
#include "M2.c"

static int testes_totais = 0;
static int testes_ok = 0;

#define VERIFICA(cond, msg) do { \
    testes_totais++; \
    if (cond) { testes_ok++; printf("  [OK]     %s\n", msg); } \
    else      { printf("  [FALHOU] %s\n", msg); } \
} while (0)

int main(void) {
    remove(ARQUIVO_BD); /* garante um banco de dados limpo antes do teste */

    printf("=== Teste 1: multiplas leituras da mesma pagina => uma unica leitura de disco (4 frames) ===\n");

    VERIFICA(cache.capacidade == 4, "capacidade do cache e 4 (padrao)");

    /* Prepara, sem usar o cache, uma pagina em disco com um registro conhecido */
    int pagina_alvo = aloca_pagina();
    VERIFICA(pagina_alvo >= 0, "pagina de teste alocada com sucesso");

    Registro original = { .id = 7, .valor = 777 };
    unsigned char bytes[TAMANHO_REGISTRO];
    serializa_registro(&original, bytes);

    unsigned char pg[TAMANHO_PAGINA];
    memset(pg, 0, TAMANHO_PAGINA);
    memcpy(pg + TAMANHO_CABECALHO, bytes, TAMANHO_REGISTRO);
    VERIFICA(escreve_pagina(pagina_alvo, pg, TAMANHO_PAGINA) == 0,
              "pagina de teste gravada diretamente em disco (fora do cache)");

    long leituras_antes = contador_leituras_disco;

    /* Primeira fixacao: deve ser FALTA e causar exatamente 1 leitura fisica */
    unsigned char *ref1 = fixa(pagina_alvo);
    VERIFICA(ref1 != NULL, "primeira fixacao da pagina retornou um buffer valido");
    VERIFICA(cache.faltas == 1, "primeira fixacao contabilizou 1 falta (miss) no cache");
    VERIFICA(contador_leituras_disco == leituras_antes + 1,
              "primeira fixacao realizou exatamente 1 leitura fisica em disco");
    solta(pagina_alvo, 0);

    /* Multiplas fixacoes adicionais da MESMA pagina (10x) */
    unsigned char *ultima_ref = NULL;
    int todas_mesma_ref = 1;
    for (int i = 0; i < 10; i++) {
        unsigned char *ref = fixa(pagina_alvo);
        if (ref == NULL) { todas_mesma_ref = 0; break; }
        if (i == 0) {
            ultima_ref = ref;
        } else if (ref != ultima_ref) {
            todas_mesma_ref = 0;
        }
        solta(pagina_alvo, 0);
    }

    VERIFICA(todas_mesma_ref && ultima_ref == ref1,
              "todas as 10 fixacoes adicionais devolveram a MESMA referencia da primeira");
    VERIFICA(contador_leituras_disco == leituras_antes + 1,
              "apos 10 fixacoes adicionais da mesma pagina, o total de leituras fisicas em disco continua sendo 1");
    VERIFICA(cache.faltas == 1, "o numero total de faltas (misses) do cache continua 1");
    VERIFICA(cache.acertos == 10, "as 10 fixacoes adicionais foram todas contabilizadas como acertos (hits)");

    /* Confirma que o conteudo continua correto apos tantos acertos */
    Registro lido;
    desserializa_registro(ref1 + TAMANHO_CABECALHO, &lido);
    VERIFICA(lido.id == 7 && lido.valor == 777, "o conteudo lido via cache permanece correto (id=7, valor=777)");

    printf("Resultado: %d/%d verificacoes passaram.\n", testes_ok, testes_totais);
    return (testes_ok == testes_totais) ? 0 : 1;
}
