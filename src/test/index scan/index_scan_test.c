/* ============================================================
 *  Testes simples do INDEX SCAN
 * ============================================================
 *
 * Cobre apenas as operacoes de index_scan.c:
 *   catalogo_listar_indices, planejamento de faixa, index_scan_abrir /
 *   index_scan_next / index_scan_fechar.
 *
 * Compilar (ajuste o caminho conforme a sua estrutura):
 *   gcc -Wall -o test_index_scan test_index_scan.c
 *   ./test_index_scan
 *
 * Este arquivo deve ficar na mesma pasta de index_scan.c.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* A demonstracao de index_scan.c usa ListaIds, mas table_scan.c so a define
   dentro de #ifdef TABLE_SCAN_MAIN (que tambem traz outro main). Definimos aqui
   para o arquivo compilar. Se mover o typedef para fora do #ifdef em
   table_scan.c, esta linha pode ser removida. */
typedef struct { int32_t ids[64]; int n; } ListaIds;

#include "../../modules/executor/scan/index_scan.c"

/* ------------------------------------------------------------------ */
/* Mini framework de testes                                            */
/* ------------------------------------------------------------------ */

static int testes_executados = 0;
static int testes_falhos     = 0;

#define VERIFICAR(cond, msg)                                                   \
    do {                                                                       \
        testes_executados++;                                                   \
        if (!(cond)) {                                                         \
            testes_falhos++;                                                   \
            printf("    [FALHOU] %s (linha %d)\n", (msg), __LINE__);           \
        }                                                                      \
    } while (0)

#define MAX_IDS 64

/* ------------------------------------------------------------------ */
/* Auxiliares                                                          */
/* ------------------------------------------------------------------ */

/* Executa o SELECT pelo Index Scan e coleta os ids emitidos.
   Retorna a quantidade de ids, ou -1 em erro. *resultado_abrir recebe o
   retorno de index_scan_abrir (0, INDEX_SCAN_NAO_APLICAVEL ou -1). */
static int coletar_ids(Catalogo *cat, const char *sql, int32_t *ids, int *resultado_abrir) {
    Parser    parser;
    Comando   cmd;
    IndexScan scan;
    Row       tupla;
    int       n = 0, r;

    iniciar_parser(&parser, sql);
    if (!parse_comando(&parser, &cmd)) return -1;

    *resultado_abrir = index_scan_abrir(&scan, cat, &cmd);
    if (*resultado_abrir < 0) return -1;
    if (*resultado_abrir == INDEX_SCAN_NAO_APLICAVEL) return 0;

    while ((r = index_scan_next(&scan, &tupla)) == 1 && n < MAX_IDS) {
        int32_t id = 0;
        row_obter_int(&tupla, 0, &id);
        ids[n++] = id;
    }
    index_scan_fechar(&scan);
    return (r < 0) ? -1 : n;
}

/* Abre o Index Scan so para inspecionar o plano escolhido. */
static int obter_plano(Catalogo *cat, const char *sql, PlanoIndexScan *plano) {
    Parser    parser;
    Comando   cmd;
    IndexScan scan;
    int       r;

    iniciar_parser(&parser, sql);
    if (!parse_comando(&parser, &cmd)) return -1;

    r = index_scan_abrir(&scan, cat, &cmd);
    *plano = scan.plano;
    index_scan_fechar(&scan);
    return r;
}

static int ids_iguais(const int32_t *obtido, int n, const int32_t *esperado, int m) {
    return n == m && memcmp(obtido, esperado, sizeof(int32_t) * (size_t)m) == 0;
}

/* ------------------------------------------------------------------ */
/* Testes                                                              */
/* ------------------------------------------------------------------ */

static void teste_catalogo_lista_indice_pk(const TabelaCat *alunos) {
    ListaIndices lista;
    printf("  catalogo_listar_indices\n");

    VERIFICAR(catalogo_listar_indices(alunos, &lista) == 1, "deve haver 1 indice");
    VERIFICAR(strcmp(lista.itens[0].nome, "pk_alunos") == 0, "nome deve ser pk_alunos");
    VERIFICAR(lista.itens[0].coluna == 0, "indice na 1a coluna");
    VERIFICAR(lista.itens[0].tipo == TOKEN_INT, "chave do tipo INT");
    VERIFICAR(catalogo_listar_indices(NULL, &lista) == 0, "tabela NULL -> 0 indices");
}

static void teste_plano_combina_condicoes(Catalogo *cat) {
    PlanoIndexScan p;
    printf("  planejamento da faixa\n");

    /* duas condicoes indexaveis viram uma unica faixa fechada */
    VERIFICAR(obter_plano(cat, "SELECT * FROM alunos WHERE id >= 30 AND id < 80;", &p) == 0,
              "abrir deve retornar 0");
    VERIFICAR(p.usa_indice && !p.vazio && !p.pontual, "faixa normal");
    VERIFICAR(p.inferior == 30 && p.superior == 79, "faixa deve ser [30, 79]");
    VERIFICAR(p.condicoes_no_indice == 2 && p.condicoes_residuais == 0, "2 condicoes no indice");

    /* igualdade e' busca pontual */
    obter_plano(cat, "SELECT * FROM alunos WHERE id = 30;", &p);
    VERIFICAR(p.pontual && p.inferior == 30 && p.superior == 30, "igualdade -> pontual [30, 30]");

    /* <> nao e' indexavel: fica como residual */
    obter_plano(cat, "SELECT * FROM alunos WHERE id >= 20 AND id <> 50;", &p);
    VERIFICAR(p.condicoes_no_indice == 1 && p.condicoes_residuais == 1, "<> deve ser residual");

    /* contradicao gera faixa vazia */
    obter_plano(cat, "SELECT * FROM alunos WHERE id > 90 AND id < 20;", &p);
    VERIFICAR(p.vazio, "id > 90 AND id < 20 -> faixa vazia");
}

static void teste_busca_pontual(Catalogo *cat) {
    int32_t ids[MAX_IDS], esperado[] = { 30 };
    int     r, n;
    printf("  busca pontual\n");

    n = coletar_ids(cat, "SELECT * FROM alunos WHERE id = 30;", ids, &r);
    VERIFICAR(r == 0 && ids_iguais(ids, n, esperado, 1), "id = 30 -> {30}");

    n = coletar_ids(cat, "SELECT * FROM alunos WHERE id = 999;", ids, &r);
    VERIFICAR(r == 0 && n == 0, "chave inexistente -> nenhuma linha");
}

static void teste_busca_por_faixa(Catalogo *cat) {
    int32_t ids[MAX_IDS];
    int32_t faixa[]  = { 30, 40, 50, 60, 70 };
    int32_t ate_35[] = { 10, 20, 30 };
    int     r, n;
    printf("  busca por faixa\n");

    n = coletar_ids(cat, "SELECT * FROM alunos WHERE id >= 30 AND id < 80;", ids, &r);
    VERIFICAR(ids_iguais(ids, n, faixa, 5), "[30, 80) -> 30..70 em ordem crescente");

    n = coletar_ids(cat, "SELECT * FROM alunos WHERE id <= 35;", ids, &r);
    VERIFICAR(ids_iguais(ids, n, ate_35, 3), "id <= 35 -> {10, 20, 30}");

    n = coletar_ids(cat, "SELECT * FROM alunos WHERE id > 90 AND id < 20;", ids, &r);
    VERIFICAR(r == 0 && n == 0, "faixa vazia -> nenhuma linha");
}

static void teste_reavaliacao_residual(Catalogo *cat) {
    int32_t ids[MAX_IDS];
    int32_t com_nome[]  = { 90 };
    int32_t sem_50[]    = { 20, 30, 40, 60, 70, 80, 90, 100 };
    int     r, n;
    printf("  reavaliacao do WHERE (condicoes residuais)\n");

    n = coletar_ids(cat, "SELECT * FROM alunos WHERE id > 40 AND nome = 'Joao';", ids, &r);
    VERIFICAR(ids_iguais(ids, n, com_nome, 1), "indice + nome = 'Joao' -> {90}");

    n = coletar_ids(cat, "SELECT * FROM alunos WHERE id >= 20 AND id <> 50;", ids, &r);
    VERIFICAR(ids_iguais(ids, n, sem_50, 8), "id >= 20 AND id <> 50 -> exclui o 50");
}

static void teste_nao_aplicavel(Catalogo *cat) {
    int32_t ids[MAX_IDS];
    int     r;
    printf("  Index Scan nao aplicavel\n");

    coletar_ids(cat, "SELECT * FROM alunos WHERE id <> 50;", ids, &r);
    VERIFICAR(r == INDEX_SCAN_NAO_APLICAVEL, "<> sozinho -> nao aplicavel");

    coletar_ids(cat, "SELECT * FROM alunos WHERE nome = 'Ana';", ids, &r);
    VERIFICAR(r == INDEX_SCAN_NAO_APLICAVEL, "coluna sem indice -> nao aplicavel");
}

static void teste_erros(Catalogo *cat) {
    int32_t   ids[MAX_IDS];
    int       r;
    Parser    parser;
    Comando   cmd;
    IndexScan scan;
    Row       tupla;
    printf("  erros\n");

    VERIFICAR(coletar_ids(cat, "SELECT * FROM fantasma WHERE id = 1;", ids, &r) == -1 && r == -1,
              "tabela inexistente -> erro");

    VERIFICAR(index_scan_abrir(NULL, cat, &cmd) == -1, "abrir com scan NULL -> erro");
    VERIFICAR(index_scan_fechar(NULL) == -1, "fechar com scan NULL -> erro");

    iniciar_parser(&parser, "SELECT * FROM alunos WHERE id = 30;");
    parse_comando(&parser, &cmd);
    VERIFICAR(index_scan_abrir(&scan, cat, &cmd) == 0, "abrir deve funcionar");
    VERIFICAR(index_scan_fechar(&scan) == 0, "fechar deve retornar 0");
    VERIFICAR(index_scan_next(&scan, &tupla) == -1, "next apos fechar -> erro");
}

/* ------------------------------------------------------------------ */
/* Carga de dados e main                                               */
/* ------------------------------------------------------------------ */

static int carregar_tabela_alunos(Catalogo *cat) {
    Coluna colunas[2];
    Row    linha;

    /* Fora de ordem, para forcar divisoes de folha na B+ */
    const int32_t ids[]   = { 50, 10, 80, 30, 70, 20, 60, 40, 90, 100 };
    const char   *nomes[] = { "Eva", "Ana", "Iris", "Caio", "Hugo", "Bia", "Gil", "Davi", "Joao", "Kai" };
    const int     total   = (int)(sizeof(ids) / sizeof(ids[0]));

    remove("alunos.dat");
    catalogo_iniciar(cat);

    strcpy(colunas[0].nome, "id");   colunas[0].tipo = TOKEN_INT;
    strcpy(colunas[1].nome, "nome"); colunas[1].tipo = TOKEN_CHAR;
    if (!catalogo_adicionar_tabela(cat, "alunos", colunas, 2)) {
        fprintf(stderr, "Erro no catalogo: %s\n", cat->mensagem);
        return -1;
    }

    const TabelaCat *alunos = catalogo_buscar_tabela(cat, "alunos");
    for (int i = 0; i < total; i++) {
        row_iniciar(&linha, alunos);
        row_definir_int(&linha, 0, ids[i]);
        row_definir_char(&linha, 1, nomes[i]);
        if (row_inserir(cat, &linha) != INSERE_OK) {
            fprintf(stderr, "Falha ao inserir a linha %d\n", i);
            return -1;
        }
    }
    return 0;
}

int main(void) {
    static Catalogo cat;

    if (carregar_tabela_alunos(&cat) != 0) return 1;

    printf("Testes do Index Scan\n");
    teste_catalogo_lista_indice_pk(catalogo_buscar_tabela(&cat, "alunos"));
    teste_plano_combina_condicoes(&cat);
    teste_busca_pontual(&cat);
    teste_busca_por_faixa(&cat);
    teste_reavaliacao_residual(&cat);
    teste_nao_aplicavel(&cat);
    teste_erros(&cat);

    remove("alunos.dat");

    printf("\n%d verificacao(oes), %d falha(s)\n", testes_executados, testes_falhos);
    return testes_falhos == 0 ? 0 : 1;
}
