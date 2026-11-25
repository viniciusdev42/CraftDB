/* ============================================================
 *              TESTES SIMPLES DO INDEX SCAN
 * ============================================================
 *
 * Coloque este arquivo na mesma pasta de index_scan.c e compile com:
 *
 *     gcc -Wall -o index_scan_test index_scan_test.c && ./index_scan_test
 *
 * Os testes usam apenas a interface publica do operador:
 *     index_scan_abrir / index_scan_next / index_scan_fechar
 * sobre a tabela alunos(id INT, nome CHAR), com 12 linhas inseridas
 * fora de ordem (varias folhas na B+, pois MAX_CHAVES = 3).
 *
 * Nao ha main de index_scan.c envolvido (so existe com -DINDEX_SCAN_MAIN).
 */

#include "../../modules/executor/scan/index_scan.c"

#define MAX_IDS 64

static int testes_total = 0;
static int testes_falhas = 0;

#define VERIFICAR(cond, msg)                                              \
    do {                                                                  \
        testes_total++;                                                   \
        if (!(cond)) {                                                    \
            testes_falhas++;                                              \
            printf("    FALHOU: %s (linha %d)\n", msg, __LINE__);         \
        }                                                                 \
    } while (0)

/* ------------------------------------------------------------------ */
/* Auxiliares                                                          */
/* ------------------------------------------------------------------ */

static Catalogo cat;

/* Insere (id, nome) usando so a API de Row (row.c). */
static int inserir_aluno(const TabelaCat *tabela, int32_t id, const char *nome) {
    Row linha;
    if (row_iniciar(&linha, tabela) != 0) return -1;
    if (row_definir_int(&linha, 0, id) != 0) return -1;
    if (row_definir_char(&linha, 1, nome) != 0) return -1;
    return row_inserir(&cat, &linha) == INSERE_OK ? 0 : -1;
}

static void preparar_banco(void) {
    Coluna colunas[2];
    const int32_t ids[]   = { 50, 10, 80, 30, 70, 20, 60, 40, 90, 100, 15, 25 };
    const char   *nomes[] = { "Eva", "Ana", "Iris", "Caio", "Hugo", "Bia",
                              "Gil", "Davi", "Joao", "Kai", "Lia", "Noe" };

    remove("alunos.dat");
    remove("idx_nome.idx");
    catalogo_iniciar(&cat);

    strcpy(colunas[0].nome, "id");   colunas[0].tipo = TOKEN_INT;
    strcpy(colunas[1].nome, "nome"); colunas[1].tipo = TOKEN_CHAR;
    if (!catalogo_adicionar_tabela(&cat, "alunos", colunas, 2)) {
        fprintf(stderr, "Erro no catalogo: %s\n", cat.mensagem);
        exit(1);
    }

    const TabelaCat *alunos = catalogo_buscar_tabela(&cat, "alunos");
    for (int i = 0; i < 12; i++) {
        if (inserir_aluno(alunos, ids[i], nomes[i]) != 0) {
            fprintf(stderr, "Falha ao inserir a linha %d\n", i);
            exit(1);
        }
    }
}

/*
 executar(sql, ids, n, scan_saida)
 Faz o parse, abre o Index Scan e coleta os ids emitidos (em ordem).
 Devolve o retorno de index_scan_abrir (0, 1 ou -1); -2 se o parse falhar
 ou se index_scan_next der erro. 'cmd' e' static para continuar vivo.
*/
static int executar(const char *sql, int32_t *ids, int *n, IndexScan *scan) {
    static Comando cmd;
    Parser parser;
    Row    row;

    *n = 0;
    iniciar_parser(&parser, sql);
    if (!parse_comando(&parser, &cmd)) return -2;

    int r = index_scan_abrir(scan, &cat, &cmd);
    if (r != 0) return r;

    int p;
    while ((p = index_scan_next(scan, &row)) == 1) {
        int32_t id = 0;
        row_obter_int(&row, 0, &id);
        if (*n < MAX_IDS) ids[(*n)++] = id;
    }
    index_scan_fechar(scan);
    return (p < 0) ? -2 : 0;
}

static int ids_iguais(const int32_t *obtido, int n, const int32_t *esperado, int m) {
    return n == m && memcmp(obtido, esperado, sizeof(int32_t) * (size_t)m) == 0;
}

/* Oraculo: Table Scan + o mesmo WHERE (predicate_evaluator.c). */
static int ids_via_table_scan(const char *sql, int32_t *ids, int *n) {
    static Comando cmd;
    Parser    parser;
    TableScan ts;
    Row       row;
    Avaliador av;
    int       r, permanece;

    *n = 0;
    iniciar_parser(&parser, sql);
    iniciar_avaliador(&av);
    if (!parse_comando(&parser, &cmd)) return -1;

    if (table_scan_abrir(&ts, &cat, cmd.tabela) != 0) return -1;
    while ((r = table_scan_next(&ts, &row)) == 1) {
        if (linha_permanece_where(&av, &cmd, &row, &permanece) != 0) {
            table_scan_fechar(&ts);
            return -1;
        }
        if (permanece) {
            int32_t id = 0;
            row_obter_int(&row, 0, &id);
            if (*n < MAX_IDS) ids[(*n)++] = id;
        }
    }
    table_scan_fechar(&ts);
    return (r < 0) ? -1 : 0;
}

static void ordenar(int32_t *v, int n) {      /* o Table Scan nao garante a ordem da chave */
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && v[j - 1] > v[j]; j--) {
            int32_t t = v[j]; v[j] = v[j - 1]; v[j - 1] = t;
        }
}

/* ------------------------------------------------------------------ */
/* Testes                                                              */
/* ------------------------------------------------------------------ */

static void teste_busca_pontual(void) {
    printf("[1] busca pontual: id = 30\n");
    int32_t ids[MAX_IDS]; int n; IndexScan scan;
    int32_t esperado[] = { 30 };

    int r = executar("SELECT * FROM alunos WHERE id = 30;", ids, &n, &scan);
    VERIFICAR(r == 0, "abrir deve devolver 0");
    VERIFICAR(ids_iguais(ids, n, esperado, 1), "deve emitir somente o id 30");
    VERIFICAR(scan.plano.pontual == 1, "plano deve ser pontual");
    VERIFICAR(scan.refs_geradas == 1, "indice deve gerar exatamente 1 referencia");
}

static void teste_conteudo_da_row(void) {
    printf("[1b] a Row emitida traz as colunas corretas (id = 30 -> Caio)\n");
    static Comando cmd;
    Parser    parser;
    IndexScan scan;
    Row       row;
    int32_t   id = 0;

    iniciar_parser(&parser, "SELECT * FROM alunos WHERE id = 30;");
    VERIFICAR(parse_comando(&parser, &cmd), "parse do SELECT");
    VERIFICAR(index_scan_abrir(&scan, &cat, &cmd) == 0, "abrir deve devolver 0");
    VERIFICAR(index_scan_next(&scan, &row) == 1, "deve emitir 1 row");

    const char *nome = row_obter_char_por_nome(&row, "nome");
    VERIFICAR(row_obter_int_por_nome(&row, "id", &id) == 0 && id == 30, "id = 30");
    VERIFICAR(nome != NULL && strcmp(nome, "Caio") == 0, "nome = 'Caio'");
    VERIFICAR(index_scan_next(&scan, &row) == 0, "segundo next deve devolver 0 (fim)");
    index_scan_fechar(&scan);
}

static void teste_faixa_fechada(void) {
    printf("[2] faixa fechada: id >= 30 AND id < 80\n");
    int32_t ids[MAX_IDS]; int n; IndexScan scan;
    int32_t esperado[] = { 30, 40, 50, 60, 70 };

    int r = executar("SELECT * FROM alunos WHERE id >= 30 AND id < 80;", ids, &n, &scan);
    VERIFICAR(r == 0, "abrir deve devolver 0");
    VERIFICAR(ids_iguais(ids, n, esperado, 5), "deve emitir 30..70 em ordem crescente");
    VERIFICAR(scan.plano.inferior == 30 && scan.plano.superior == 79,
              "limites do plano devem ser [30, 79]");
    VERIFICAR(scan.descartadas == 0, "nada deve ser descartado na reavaliacao");
}

static void teste_faixa_com_residual(void) {
    printf("[3] faixa + condicao residual: id > 20 AND nome <> 'Eva'\n");
    int32_t ids[MAX_IDS]; int n; IndexScan scan;
    int32_t esperado[] = { 25, 30, 40, 60, 70, 80, 90, 100 };   /* 50 = Eva sai */

    int r = executar("SELECT * FROM alunos WHERE id > 20 AND nome <> 'Eva';", ids, &n, &scan);
    VERIFICAR(r == 0, "abrir deve devolver 0");
    VERIFICAR(ids_iguais(ids, n, esperado, 8), "id 50 (Eva) deve ser removido pela reavaliacao");
    VERIFICAR(scan.plano.condicoes_residuais == 1, "'<>' deve ser residual");
    VERIFICAR(scan.descartadas == 1, "exatamente 1 linha reprovada na reavaliacao");
}

static void teste_faixa_vazia(void) {
    printf("[4] faixa vazia: id > 90 AND id < 20\n");
    int32_t ids[MAX_IDS]; int n; IndexScan scan;

    int r = executar("SELECT * FROM alunos WHERE id > 90 AND id < 20;", ids, &n, &scan);
    VERIFICAR(r == 0, "abrir deve devolver 0");
    VERIFICAR(n == 0, "nenhuma linha deve ser emitida");
    VERIFICAR(scan.plano.vazio == 1, "plano deve marcar faixa vazia");
    VERIFICAR(scan.registros_lidos == 0, "nenhum registro deve ser lido");
}

static void teste_limites_da_faixa(void) {
    printf("[5] limites: id <= 15 e id > 100\n");
    int32_t ids[MAX_IDS]; int n; IndexScan scan;
    int32_t esperado[] = { 10, 15 };

    int r = executar("SELECT * FROM alunos WHERE id <= 15;", ids, &n, &scan);
    VERIFICAR(r == 0 && ids_iguais(ids, n, esperado, 2), "id <= 15 deve emitir 10 e 15");

    r = executar("SELECT * FROM alunos WHERE id > 100;", ids, &n, &scan);
    VERIFICAR(r == 0 && n == 0, "id > 100 nao deve emitir nada");
}

static void teste_equivale_ao_table_scan(void) {
    printf("[6] Index Scan == Table Scan + WHERE\n");
    const char *consultas[] = {
        "SELECT * FROM alunos WHERE id = 30;",
        "SELECT * FROM alunos WHERE id >= 20 AND id <= 70;",
        "SELECT * FROM alunos WHERE id > 15 AND id < 90 AND nome <> 'Eva';",
        "SELECT * FROM alunos WHERE id < 40 AND id > 10;",
        "SELECT * FROM alunos WHERE id > 90 AND id < 20;",
        "SELECT * FROM alunos WHERE id >= 25 AND nome = 'Hugo';",
    };
    for (size_t i = 0; i < sizeof consultas / sizeof consultas[0]; i++) {
        int32_t via_indice[MAX_IDS], via_scan[MAX_IDS];
        int n1, n2;
        IndexScan scan;

        int r1 = executar(consultas[i], via_indice, &n1, &scan);
        int r2 = ids_via_table_scan(consultas[i], via_scan, &n2);
        ordenar(via_scan, n2);

        printf("    %s\n", consultas[i]);
        VERIFICAR(r1 == 0 && r2 == 0, "ambas as varreduras devem executar sem erro");
        VERIFICAR(ids_iguais(via_indice, n1, via_scan, n2), "mesmo conjunto de ids");
    }
}

static void teste_nao_aplicavel(void) {
    printf("[7] sem predicado indexavel -> INDEX_SCAN_NAO_APLICAVEL\n");
    int32_t ids[MAX_IDS]; int n; IndexScan scan;

    VERIFICAR(executar("SELECT * FROM alunos WHERE id <> 30;", ids, &n, &scan)
              == INDEX_SCAN_NAO_APLICAVEL, "'<>' nao e' indexavel");
    VERIFICAR(executar("SELECT * FROM alunos WHERE nome = 'Ana';", ids, &n, &scan)
              == INDEX_SCAN_NAO_APLICAVEL, "coluna sem indice executavel");
    VERIFICAR(executar("SELECT * FROM alunos;", ids, &n, &scan)
              == INDEX_SCAN_NAO_APLICAVEL, "sem WHERE");
}

static void teste_erros(void) {
    printf("[8] erros\n");
    int32_t ids[MAX_IDS]; int n; IndexScan scan;

    VERIFICAR(executar("SELECT * FROM fantasma WHERE id = 1;", ids, &n, &scan) == -1,
              "tabela inexistente deve devolver -1");
    VERIFICAR(executar("SELECT * FROM alunos WHERE id = 'abc';", ids, &n, &scan) == -1,
              "tipo do literal incompativel deve devolver -1");

    Row row;
    memset(&scan, 0, sizeof scan);
    VERIFICAR(index_scan_next(&scan, &row) == -1, "next em varredura nao aberta deve devolver -1");
    VERIFICAR(index_scan_abrir(NULL, &cat, NULL) == -1, "argumentos NULL devem devolver -1");
}

/* ------------------------------------------------------------------ */

int main(void) {
    preparar_banco();

    teste_busca_pontual();
    teste_conteudo_da_row();
    teste_faixa_fechada();
    teste_faixa_com_residual();
    teste_faixa_vazia();
    teste_limites_da_faixa();
    teste_equivale_ao_table_scan();
    teste_nao_aplicavel();
    teste_erros();

    remove("alunos.dat");
    remove("idx_nome.idx");

    printf("\n%d verificacao(oes), %d falha(s)\n", testes_total, testes_falhas);
    return testes_falhas == 0 ? 0 : 1;
}