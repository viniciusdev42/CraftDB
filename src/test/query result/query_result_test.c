/* ============================================================
 *              TESTES DO QUERY RESULT
 * ============================================================
 *
 * Testa apenas as funcoes de QueryResult.c. Row e Catalogo sao usados
 * somente para montar os dados de entrada.
 *
 * Compilar (na mesma pasta de QueryResult.c e Row.c):
 *     gcc TestQueryResult.c -o test_query_result
 *
 * As mensagens "Erro: ..." impressas em stderr sao ESPERADAS nos testes
 * de caso invalido. O que importa e' o resumo final.
 */
#include "../../modules/executor/result/query_result.c"

/* ------------------------------------------------------------------ */
/* Mini framework                                                      */
/* ------------------------------------------------------------------ */

static int total_checks = 0;
static int falhas       = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        total_checks++;                                                      \
        if (!(cond)) {                                                       \
            falhas++;                                                        \
            printf("    [FALHA] %s (linha %d)\n", #cond, __LINE__);          \
        }                                                                    \
    } while (0)

#define RUN(teste)                                                           \
    do {                                                                     \
        int antes = falhas;                                                  \
        teste();                                                             \
        printf("[%s] %s\n", falhas == antes ? " OK " : "FAIL", #teste);      \
    } while (0)

/* ------------------------------------------------------------------ */
/* Dados de apoio: tabela alunos (id INT, nome CHAR)                   */
/* ------------------------------------------------------------------ */

static Catalogo cat;
static const TabelaCat *tabela;

static void preparar_catalogo(void) {
    Coluna colunas[2];

    remove("alunos.dat");
    catalogo_iniciar(&cat);

    strcpy(colunas[0].nome, "id");   colunas[0].tipo = TOKEN_INT;
    strcpy(colunas[1].nome, "nome"); colunas[1].tipo = TOKEN_CHAR;
    catalogo_adicionar_tabela(&cat, "alunos", colunas, 2);
    tabela = catalogo_buscar_tabela(&cat, "alunos");
}

static void montar_row(Row *row, int32_t id, const char *nome) {
    row_iniciar(row, tabela);
    row_definir_int(row, 0, id);
    row_definir_char(row, 1, nome);
}

/* Filtro: mantem linhas com id >= *arg */
static int id_minimo(const Row *row, void *arg) {
    int32_t id = 0;
    row_obter_int(row, 0, &id);
    return id >= *(const int32_t *)arg;
}

/* ------------------------------------------------------------------ */
/* Testes                                                              */
/* ------------------------------------------------------------------ */

/* qr_iniciar: respeita a ordem pedida e ignora maiusculas/minusculas */
static void teste_iniciar_com_projecao(void) {
    QueryResult qr;
    const char *nomes[] = { "nome", "ID" };

    CHECK(qr_iniciar(&qr, tabela, nomes, 2) == 0);
    CHECK(qr_numero_colunas(&qr) == 2);
    CHECK(strcmp(qr_coluna(&qr, 0)->nome, "nome") == 0);
    CHECK(qr_coluna(&qr, 0)->tipo == TOKEN_CHAR);
    CHECK(qr_coluna(&qr, 0)->posicao_origem == 1);
    CHECK(strcmp(qr_coluna(&qr, 1)->nome, "id") == 0);   /* grafia do catalogo */
    CHECK(qr_coluna(&qr, 1)->posicao_origem == 0);
    CHECK(qr_posicao_da_coluna(&qr, "id") == 1);
    CHECK(qr_posicao_da_coluna(&qr, "idade") == -1);
    qr_liberar(&qr);
}

/* qr_iniciar: NULL/0 equivale a SELECT * */
static void teste_iniciar_select_all(void) {
    QueryResult qr;

    CHECK(qr_iniciar(&qr, tabela, NULL, 0) == 0);
    CHECK(qr_numero_colunas(&qr) == 2);
    CHECK(strcmp(qr_coluna(&qr, 0)->nome, "id") == 0);
    CHECK(strcmp(qr_coluna(&qr, 1)->nome, "nome") == 0);
    qr_liberar(&qr);
}

/* qr_iniciar: argumentos e colunas invalidos */
static void teste_iniciar_invalido(void) {
    QueryResult qr;
    const char *inexistente[] = { "id", "idade" };

    CHECK(qr_iniciar(&qr, tabela, inexistente, 2) == -1);
    CHECK(qr_numero_colunas(&qr) == 0);
    CHECK(qr.linhas == NULL);
    CHECK(qr_iniciar(NULL, tabela, NULL, 0) == -1);
    CHECK(qr_iniciar(&qr, NULL, NULL, 0) == -1);
}

/* qr_adicionar_row + qr_linha + qr_obter_*: valores por posicao e por nome */
static void teste_adicionar_e_ler_valores(void) {
    QueryResult qr;
    Row row;
    const char *nomes[] = { "nome", "id" };
    int32_t id = 0;

    qr_iniciar(&qr, tabela, nomes, 2);
    montar_row(&row, 10, "Ana");
    CHECK(qr_adicionar_row(&qr, &row) == 0);
    montar_row(&row, 20, "Bia");
    CHECK(qr_adicionar_row(&qr, &row) == 0);
    CHECK(qr_numero_linhas(&qr) == 2);

    const LinhaResultado *l1 = qr_linha(&qr, 1);
    CHECK(l1 != NULL);
    CHECK(strcmp(qr_obter_char(&qr, l1, 0), "Bia") == 0);             /* posicao */
    CHECK(qr_obter_int(&qr, l1, 1, &id) == 0 && id == 20);
    CHECK(strcmp(qr_obter_char_por_nome(&qr, l1, "NOME"), "Bia") == 0); /* nome   */
    CHECK(qr_obter_int_por_nome(&qr, l1, "id", &id) == 0 && id == 20);
    qr_liberar(&qr);
}

/* Leituras invalidas: tipo errado, coluna/linha inexistente */
static void teste_leituras_invalidas(void) {
    QueryResult qr;
    Row row;
    int32_t v = 0;

    qr_iniciar(&qr, tabela, NULL, 0);
    montar_row(&row, 10, "Ana");
    qr_adicionar_row(&qr, &row);
    const LinhaResultado *l = qr_linha(&qr, 0);

    CHECK(qr_obter_int(&qr, l, 1, &v) == -1);            /* nome lido como INT  */
    CHECK(qr_obter_char(&qr, l, 0) == NULL);             /* id lido como CHAR   */
    CHECK(qr_obter_int(&qr, l, 5, &v) == -1);            /* posicao invalida    */
    CHECK(qr_obter_char_por_nome(&qr, l, "idade") == NULL);
    CHECK(qr_linha(&qr, 1) == NULL);                     /* indice fora do fim  */
    CHECK(qr_linha(&qr, -1) == NULL);
    qr_liberar(&qr);
}

/* qr_adicionar_row: rejeita Row de outra tabela */
static void teste_adicionar_row_de_outra_tabela(void) {
    QueryResult qr;
    Row row;

    qr_iniciar(&qr, tabela, NULL, 0);
    montar_row(&row, 10, "Ana");
    strcpy(row.tabela, "outra");

    CHECK(qr_adicionar_row(&qr, &row) == -1);
    CHECK(qr_numero_linhas(&qr) == 0);
    CHECK(qr_adicionar_row(&qr, NULL) == -1);
    qr_liberar(&qr);
}

/* Capacidade cresce alem da inicial (8 linhas) sem perder dados */
static void teste_crescimento_do_vetor(void) {
    QueryResult qr;
    Row row;
    int32_t id = 0;

    qr_iniciar(&qr, tabela, NULL, 0);
    for (int i = 0; i < 20; i++) {
        montar_row(&row, i, "x");
        CHECK(qr_adicionar_row(&qr, &row) == 0);
    }
    CHECK(qr_numero_linhas(&qr) == 20);
    CHECK(qr_obter_int(&qr, qr_linha(&qr, 19), 0, &id) == 0 && id == 19);
    qr_liberar(&qr);
}

/* Cursor: qr_proxima percorre tudo, devolve NULL no fim, e reinicia */
static void teste_cursor(void) {
    QueryResult qr;
    Row row;
    int lidas = 0;

    qr_iniciar(&qr, tabela, NULL, 0);
    for (int i = 0; i < 3; i++) {
        montar_row(&row, i, "x");
        qr_adicionar_row(&qr, &row);
    }

    while (qr_proxima(&qr) != NULL) lidas++;
    CHECK(lidas == 3);
    CHECK(qr_proxima(&qr) == NULL);

    qr_reiniciar_cursor(&qr);
    CHECK(qr_proxima(&qr) == qr_linha(&qr, 0));
    qr_liberar(&qr);
}

/* qr_liberar: zera o resultado e pode ser chamada de novo */
static void teste_liberar(void) {
    QueryResult qr;
    Row row;

    qr_iniciar(&qr, tabela, NULL, 0);
    montar_row(&row, 1, "Ana");
    qr_adicionar_row(&qr, &row);

    qr_liberar(&qr);
    CHECK(qr.linhas == NULL);
    CHECK(qr_numero_linhas(&qr) == 0);
    CHECK(qr_numero_colunas(&qr) == 0);
    qr_liberar(&qr);     /* segunda chamada nao pode quebrar */
    qr_liberar(NULL);
}

/* Funcoes com ponteiro NULL */
static void teste_ponteiro_nulo(void) {
    CHECK(qr_numero_colunas(NULL) == -1);
    CHECK(qr_numero_linhas(NULL) == -1);
    CHECK(qr_coluna(NULL, 0) == NULL);
    CHECK(qr_linha(NULL, 0) == NULL);
    CHECK(qr_proxima(NULL) == NULL);
    CHECK(qr_posicao_da_coluna(NULL, "id") == -1);
    qr_reiniciar_cursor(NULL);
}

/* qr_executar: le da tabela (B+/disco), com projecao e filtro */
static void teste_executar(void) {
    QueryResult qr;
    Row row;
    const char *nomes[] = { "nome" };
    int32_t minimo = 30, id = 0;

    for (int i = 1; i <= 5; i++) {
        montar_row(&row, 10 * i, i % 2 ? "Impar" : "Par");
        CHECK(row_inserir(&cat, &row) == INSERE_OK);
    }

    /* sem filtro, SELECT * */
    CHECK(qr_executar(&cat, "alunos", NULL, 0, NULL, NULL, &qr) == 5);
    CHECK(qr_obter_int(&qr, qr_linha(&qr, 0), 0, &id) == 0 && id == 10);
    CHECK(qr_obter_int(&qr, qr_linha(&qr, 4), 0, &id) == 0 && id == 50);
    qr_liberar(&qr);

    /* com filtro e projecao de uma coluna */
    CHECK(qr_executar(&cat, "alunos", nomes, 1, id_minimo, &minimo, &qr) == 3);
    CHECK(qr_numero_colunas(&qr) == 1);
    CHECK(strcmp(qr_obter_char(&qr, qr_linha(&qr, 0), 0), "Impar") == 0);   /* id 30 */
    qr_liberar(&qr);
}

/* qr_executar: erros nao deixam memoria alocada */
static void teste_executar_invalido(void) {
    QueryResult qr;
    const char *inexistente[] = { "idade" };

    CHECK(qr_executar(&cat, "fantasma", NULL, 0, NULL, NULL, &qr) == -1);
    CHECK(qr_executar(&cat, "alunos", inexistente, 1, NULL, NULL, &qr) == -1);
    CHECK(qr.linhas == NULL);
    CHECK(qr_executar(NULL, "alunos", NULL, 0, NULL, NULL, &qr) == -1);
}

/* ------------------------------------------------------------------ */

int main(void) {
    preparar_catalogo();
    if (tabela == NULL) {
        fprintf(stderr, "Falha ao montar o catalogo de teste: %s\n", cat.mensagem);
        return 1;
    }

    RUN(teste_iniciar_com_projecao);
    RUN(teste_iniciar_select_all);
    RUN(teste_iniciar_invalido);
    RUN(teste_adicionar_e_ler_valores);
    RUN(teste_leituras_invalidas);
    RUN(teste_adicionar_row_de_outra_tabela);
    RUN(teste_crescimento_do_vetor);
    RUN(teste_cursor);
    RUN(teste_liberar);
    RUN(teste_ponteiro_nulo);
    RUN(teste_executar);
    RUN(teste_executar_invalido);

    remove("alunos.dat");
    printf("\n%d verificacao(oes), %d falha(s).\n", total_checks, falhas);
    return falhas == 0 ? 0 : 1;
}
