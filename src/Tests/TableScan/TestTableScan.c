/* ============================================================
 *            TESTES DO TABLE SCAN (sem frameworks externos)
 * ============================================================
 * Compilar e executar (na mesma pasta de TableScan.c, Row.c, ...):
 *     gcc -Wall -o test_table_scan TestTableScan.c && ./test_table_scan
 *
 * Cobre apenas as operacoes de TableScan.c:
 *     table_scan_abrir / table_scan_next / table_scan_fechar
 * (Row, Catalog, Parser/Tokenizer, BPlusTree, Cache e Pager sao usados apenas como apoio:
 * o cenario e' montado com comandos SQL via Parser + Catalog.)
 */

/* Evita os main() de Row.c e de TableScan.c: este arquivo tem o seu proprio */
#define ROW_MAIN
#define TABLE_SCAN_MAIN
#include "TableScan.c"

/* ------------------------------------------------------------------ */
/* Mini-framework de asserts                                           */
/* ------------------------------------------------------------------ */

static int testes_total = 0;
static int testes_falhas = 0;

#define CHECK(cond, msg) do {                                            \
    testes_total++;                                                      \
    if (!(cond)) {                                                       \
        testes_falhas++;                                                 \
        printf("    [FALHA] %s (linha %d)\n", msg, __LINE__);            \
    }                                                                    \
} while (0)

/* ------------------------------------------------------------------ */
/* Apoio                                                               */
/* ------------------------------------------------------------------ */

static Catalogo cat;

/* Executa um comando SQL (Tokenizer + Parser + Catalog), usado so para montar o cenario */
static int sql(const char *texto) {
    Parser parser;
    Comando comando;
    iniciar_parser(&parser, texto);
    if (!parse_comando(&parser, &comando)) return 0;
    return catalogo_executar(&cat, &comando);
}

/* Cria o catalogo com 'alunos' (id INT, nome CHAR) e 'vazia' (id INT, valor INT)
   e remove arquivos de dados de execucoes anteriores. */
static void preparar(void) {
    remove("alunos.dat");
    remove("vazia.dat");
    reiniciar_cache();
    catalogo_iniciar(&cat);

    sql("CREATE TABLE alunos (id INT, nome CHAR);");
    sql("CREATE TABLE vazia (id INT, valor INT);");
}

static void limpar(void) {
    reiniciar_cache();
    remove("alunos.dat");
    remove("vazia.dat");
}

/* Insere 'n' alunos (ids na ordem dada, nome fixo) */
static void inserir_alunos(const int32_t *ids, const char **nomes, int n) {
    const TabelaCat *t = catalogo_buscar_tabela(&cat, "alunos");
    Row linha;
    for (int i = 0; i < n; i++) {
        row_iniciar(&linha, t);
        row_definir_int(&linha, 0, ids[i]);
        row_definir_char(&linha, 1, nomes[i]);
        row_inserir(&cat, &linha);
    }
}

/* ------------------------------------------------------------------ */
/* Testes                                                              */
/* ------------------------------------------------------------------ */

/* 1) Entrega todas as tuplas, em ordem crescente de id, com os valores corretos
      (10 registros fora de ordem forcam varias folhas, pois MAX_CHAVES = 3). */
static void teste_varredura_completa_e_ordenada(void) {
    printf("teste_varredura_completa_e_ordenada\n");
    preparar();

    const int32_t ids[]   = { 50, 10, 80, 30, 70, 20, 60, 40, 90, 100 };
    const char   *nomes[] = { "Eva", "Ana", "Iris", "Caio", "Hugo", "Bia", "Gil", "Davi", "Joao", "Kai" };
    const int32_t esperado_ids[]   = { 10, 20, 30, 40, 50, 60, 70, 80, 90, 100 };
    const char   *esperado_nomes[] = { "Ana", "Bia", "Caio", "Davi", "Eva", "Gil", "Hugo", "Iris", "Joao", "Kai" };
    inserir_alunos(ids, nomes, 10);

    TableScan scan;
    Row tupla;
    int n = 0, r;
    int ordem_ok = 1, valores_ok = 1;

    CHECK(table_scan_abrir(&scan, &cat, "alunos") == 0, "abrir deve retornar 0");
    while ((r = table_scan_next(&scan, &tupla)) == 1) {
        int32_t id = -1;
        row_obter_int(&tupla, 0, &id);
        const char *nome = row_obter_char(&tupla, 1);
        if (n >= 10 || id != esperado_ids[n]) ordem_ok = 0;
        if (n >= 10 || nome == NULL || strcmp(nome, esperado_nomes[n]) != 0) valores_ok = 0;
        n++;
    }
    CHECK(r == 0, "varredura deve terminar com 0 (fim), nao -1");
    CHECK(n == 10, "devem ser entregues exatamente 10 tuplas");
    CHECK(ordem_ok, "tuplas devem vir em ordem crescente de id");
    CHECK(valores_ok, "cada tupla deve trazer o nome correto");
    CHECK(table_scan_fechar(&scan) == 0, "fechar deve retornar 0");

    limpar();
}

/* 2) Tabela com poucos registros (uma unica folha, com slots livres ignorados) */
static void teste_uma_folha_com_slots_livres(void) {
    printf("teste_uma_folha_com_slots_livres\n");
    preparar();

    const int32_t ids[]   = { 2, 1 };
    const char   *nomes[] = { "Bia", "Ana" };
    inserir_alunos(ids, nomes, 2);

    TableScan scan;
    Row tupla;
    int32_t id = 0;

    table_scan_abrir(&scan, &cat, "alunos");
    CHECK(table_scan_next(&scan, &tupla) == 1, "1a chamada entrega tupla");
    row_obter_int(&tupla, 0, &id);
    CHECK(id == 1, "1a tupla tem id 1");
    CHECK(table_scan_next(&scan, &tupla) == 1, "2a chamada entrega tupla");
    row_obter_int(&tupla, 0, &id);
    CHECK(id == 2, "2a tupla tem id 2");
    CHECK(table_scan_next(&scan, &tupla) == 0, "3a chamada indica fim (slot livre ignorado)");
    table_scan_fechar(&scan);

    limpar();
}

/* 3) Tabela vazia: fim imediato */
static void teste_tabela_vazia(void) {
    printf("teste_tabela_vazia\n");
    preparar();

    TableScan scan;
    Row tupla;

    CHECK(table_scan_abrir(&scan, &cat, "vazia") == 0, "abrir tabela vazia deve funcionar");
    CHECK(table_scan_next(&scan, &tupla) == 0, "next em tabela vazia retorna 0");
    CHECK(table_scan_fechar(&scan) == 0, "fechar deve retornar 0");

    limpar();
}

/* 4) Depois do fim, next continua devolvendo 0 e nao deixa pin pendente */
static void teste_next_apos_o_fim(void) {
    printf("teste_next_apos_o_fim\n");
    preparar();

    const int32_t ids[]   = { 3, 1, 2 };
    const char   *nomes[] = { "Caio", "Ana", "Bia" };
    inserir_alunos(ids, nomes, 3);

    TableScan scan;
    Row tupla;

    table_scan_abrir(&scan, &cat, "alunos");
    while (table_scan_next(&scan, &tupla) == 1) { }
    CHECK(table_scan_next(&scan, &tupla) == 0, "next apos o fim retorna 0");
    CHECK(table_scan_next(&scan, &tupla) == 0, "next repetido apos o fim continua 0");
    CHECK(reiniciar_cache() == 0, "nenhum pin deve restar ao chegar no fim");
    table_scan_fechar(&scan);

    limpar();
}

/* 5) Desistir no meio: o pin existe ate fechar, e fechar o libera */
static void teste_fechar_no_meio_libera_pin(void) {
    printf("teste_fechar_no_meio_libera_pin\n");
    preparar();

    const int32_t ids[]   = { 50, 10, 80, 30, 70, 20 };
    const char   *nomes[] = { "Eva", "Ana", "Iris", "Caio", "Hugo", "Bia" };
    inserir_alunos(ids, nomes, 6);

    TableScan scan;
    Row tupla;

    table_scan_abrir(&scan, &cat, "alunos");
    CHECK(table_scan_next(&scan, &tupla) == 1, "recebe a 1a tupla");
    CHECK(reiniciar_cache() != 0, "cache nao reinicia com varredura parada no meio (pagina fixada)");
    CHECK(table_scan_fechar(&scan) == 0, "fechar no meio retorna 0");
    CHECK(reiniciar_cache() == 0, "cache reinicia apos fechar (pin liberado)");
    CHECK(table_scan_fechar(&scan) == 0, "fechar duas vezes e' seguro");

    limpar();
}

/* 6) Erros de uso */
static void teste_erros(void) {
    printf("teste_erros\n");
    preparar();

    TableScan scan;
    Row tupla;

    CHECK(table_scan_abrir(&scan, &cat, "fantasma") == -1, "abrir tabela inexistente retorna -1");
    CHECK(table_scan_abrir(NULL, &cat, "alunos") == -1, "abrir com scan NULL retorna -1");
    CHECK(table_scan_abrir(&scan, NULL, "alunos") == -1, "abrir com catalogo NULL retorna -1");
    CHECK(table_scan_abrir(&scan, &cat, NULL) == -1, "abrir com nome NULL retorna -1");

    memset(&scan, 0, sizeof(scan));
    CHECK(table_scan_next(&scan, &tupla) == -1, "next em varredura nao aberta retorna -1");

    table_scan_abrir(&scan, &cat, "alunos");
    CHECK(table_scan_next(&scan, NULL) == -1, "next com saida NULL retorna -1");
    CHECK(table_scan_next(NULL, &tupla) == -1, "next com scan NULL retorna -1");
    table_scan_fechar(&scan);
    CHECK(table_scan_fechar(NULL) == -1, "fechar com scan NULL retorna -1");

    limpar();
}

/* 7) Nome da tabela sem diferenciar maiusculas/minusculas (regra do Catalog) */
static void teste_nome_da_tabela_case_insensitive(void) {
    printf("teste_nome_da_tabela_case_insensitive\n");
    preparar();
    sql("INSERT INTO alunos VALUES (1, 'Ana');");

    TableScan scan;
    Row tupla;

    CHECK(table_scan_abrir(&scan, &cat, "ALUNOS") == 0, "abrir 'ALUNOS' encontra a tabela 'alunos'");
    CHECK(table_scan_next(&scan, &tupla) == 1, "entrega a tupla");
    table_scan_fechar(&scan);

    limpar();
}

/* 8) Varreduras em sequencia sobre tabelas diferentes (cada uma no seu arquivo),
      incluindo coluna INT no lugar de CHAR */
static void teste_duas_tabelas_em_sequencia(void) {
    printf("teste_duas_tabelas_em_sequencia\n");
    preparar();
    sql("INSERT INTO alunos VALUES (2, 'Bia');");
    sql("INSERT INTO alunos VALUES (1, 'Ana');");
    sql("INSERT INTO vazia VALUES (7, 700);");
    sql("INSERT INTO vazia VALUES (5, 500);");

    TableScan scan;
    Row tupla;
    int32_t id = 0, valor = 0;

    table_scan_abrir(&scan, &cat, "alunos");
    int n = 0;
    while (table_scan_next(&scan, &tupla) == 1) n++;
    CHECK(n == 2, "'alunos' entrega 2 tuplas");
    table_scan_fechar(&scan);

    CHECK(table_scan_abrir(&scan, &cat, "vazia") == 0, "abrir a 2a tabela");
    CHECK(table_scan_next(&scan, &tupla) == 1, "1a tupla da 2a tabela");
    row_obter_int(&tupla, 0, &id);
    row_obter_int(&tupla, 1, &valor);
    CHECK(id == 5 && valor == 500, "1a tupla: (5, 500)");
    CHECK(table_scan_next(&scan, &tupla) == 1, "2a tupla da 2a tabela");
    row_obter_int(&tupla, 0, &id);
    row_obter_int(&tupla, 1, &valor);
    CHECK(id == 7 && valor == 700, "2a tupla: (7, 700)");
    CHECK(table_scan_next(&scan, &tupla) == 0, "fim da 2a tabela");
    table_scan_fechar(&scan);

    limpar();
}

int main(void) {
    teste_varredura_completa_e_ordenada();
    teste_uma_folha_com_slots_livres();
    teste_tabela_vazia();
    teste_next_apos_o_fim();
    teste_fechar_no_meio_libera_pin();
    teste_erros();
    teste_nome_da_tabela_case_insensitive();
    teste_duas_tabelas_em_sequencia();

    printf("\n%d verificacoes, %d falha(s)\n", testes_total, testes_falhas);
    return testes_falhas == 0 ? 0 : 1;
}
