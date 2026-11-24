/* ============================================================
 *            TESTES DO QUERY EXECUTOR
 * ============================================================
 *
 * Testa apenas as funcoes do query_executor.c:
 *   - iniciar_executor()
 *   - executar_consulta()   (validacao, varredura, WHERE, projecao, estatisticas)
 *
 * Compilacao (a partir da raiz do projeto, onde existe a pasta src/):
 *   gcc -Wall -Wextra -Isrc -o testes tests/query_executor_tests.c && ./testes
 *
 * Nao defina nenhuma macro *_MAIN: elas ligam os mains de demonstracao dos
 * modulos e colidiriam com o main deste arquivo.
 */

#include "../../modules/executor/executor/query_executor.c"

/* ------------------------------------------------------------------ */
/* Mini framework                                                      */
/* ------------------------------------------------------------------ */

static int total_testes = 0;
static int falhas       = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        total_testes++;                                                      \
        if (!(cond)) {                                                       \
            falhas++;                                                        \
            printf("    FALHOU (linha %d): %s\n", __LINE__, #cond);          \
        }                                                                    \
    } while (0)

/* ------------------------------------------------------------------ */
/* Ambiente: tabela alunos(id INT, nome CHAR) com 5 linhas             */
/* ------------------------------------------------------------------ */

static Catalogo cat;
static Executor ex;

static int montar_ambiente(void)
{
    Coluna colunas[2];
    Row    linha;
    const int32_t ids[]   = { 50, 10, 30, 20, 40 };           /* fora de ordem */
    const char   *nomes[] = { "Eva", "Ana", "Caio", "Bia", "Davi" };

    remove("alunos.dat");
    catalogo_iniciar(&cat);
    iniciar_executor(&ex);

    strcpy(colunas[0].nome, "id");   colunas[0].tipo = TOKEN_INT;
    strcpy(colunas[1].nome, "nome"); colunas[1].tipo = TOKEN_CHAR;
    if (!catalogo_adicionar_tabela(&cat, "alunos", colunas, 2))
        return -1;

    const TabelaCat *alunos = catalogo_buscar_tabela(&cat, "alunos");
    for (int i = 0; i < 5; i++) {
        row_iniciar(&linha, alunos);
        row_definir_int(&linha, 0, ids[i]);
        row_definir_char(&linha, 1, nomes[i]);
        if (row_inserir(&cat, &linha) != INSERE_OK)
            return -1;
    }
    return 0;
}

/* Parseia 'sql' e executa. Devolve o retorno de executar_consulta (ou -2 se o
   SQL nao for sintaticamente valido). */
static int rodar(const char *sql, const char *const *proj, int n, QueryResult *r)
{
    Parser  parser;
    Comando cmd;

    iniciar_parser(&parser, sql);
    if (!parse_comando(&parser, &cmd))
        return -2;
    return executar_consulta(&ex, &cat, &cmd, proj, n, r);
}

/* ------------------------------------------------------------------ */
/* Testes                                                              */
/* ------------------------------------------------------------------ */

/* iniciar_executor zera mensagem e estatisticas. */
static void teste_iniciar_executor(void)
{
    Executor e;
    printf("- iniciar_executor\n");

    memset(&e, 0xFF, sizeof e);
    iniciar_executor(&e);
    CHECK(e.mensagem[0] == '\0');
    CHECK(e.linhas_varridas == 0);
    CHECK(e.linhas_descartadas == 0);
}

/* SELECT * sem WHERE: todas as linhas, em ordem crescente da chave. */
static void teste_select_todas_as_linhas(void)
{
    QueryResult r;
    int32_t     id = 0;
    printf("- SELECT * sem WHERE\n");

    CHECK(rodar("SELECT * FROM alunos;", NULL, 0, &r) == 5);
    CHECK(qr_numero_linhas(&r) == 5);
    CHECK(qr_numero_colunas(&r) == 2);
    CHECK(ex.linhas_varridas == 5);
    CHECK(ex.linhas_descartadas == 0);

    CHECK(qr_obter_int_por_nome(&r, qr_linha(&r, 0), "id", &id) == 0 && id == 10);
    CHECK(qr_obter_int_por_nome(&r, qr_linha(&r, 4), "id", &id) == 0 && id == 50);
    CHECK(strcmp(qr_obter_char_por_nome(&r, qr_linha(&r, 0), "nome"), "Ana") == 0);

    qr_liberar(&r);
}

/* WHERE com AND: filtra e conta varridas/descartadas. */
static void teste_where(void)
{
    QueryResult r;
    int32_t     id = 0;
    printf("- WHERE\n");

    CHECK(rodar("SELECT * FROM alunos WHERE id >= 20 AND id < 50;", NULL, 0, &r) == 3);
    CHECK(ex.linhas_varridas == 5);
    CHECK(ex.linhas_descartadas == 2);
    CHECK(qr_obter_int_por_nome(&r, qr_linha(&r, 0), "id", &id) == 0 && id == 20);
    CHECK(qr_obter_int_por_nome(&r, qr_linha(&r, 2), "id", &id) == 0 && id == 40);
    qr_liberar(&r);

    /* nenhuma linha satisfaz: resultado vazio, mas NAO e' erro */
    CHECK(rodar("SELECT * FROM alunos WHERE id > 1000;", NULL, 0, &r) == 0);
    CHECK(ex.linhas_descartadas == 5);
    qr_liberar(&r);

    /* comparacao de CHAR */
    CHECK(rodar("SELECT * FROM alunos WHERE nome = 'Caio';", NULL, 0, &r) == 1);
    qr_liberar(&r);
}

/* Projecao: ordem pedida e nomes sem diferenciar maiusculas/minusculas. */
static void teste_projecao(void)
{
    QueryResult r;
    int32_t     id = 0;
    const char *proj[] = { "nome", "ID" };
    printf("- projecao de colunas\n");

    CHECK(rodar("SELECT * FROM alunos WHERE id <= 20;", proj, 2, &r) == 2);
    CHECK(qr_numero_colunas(&r) == 2);
    CHECK(qr_posicao_da_coluna(&r, "nome") == 0);   /* nome vem primeiro */
    CHECK(qr_posicao_da_coluna(&r, "id") == 1);
    CHECK(strcmp(qr_obter_char(&r, qr_linha(&r, 0), 0), "Ana") == 0);
    CHECK(qr_obter_int(&r, qr_linha(&r, 0), 1, &id) == 0 && id == 10);
    qr_liberar(&r);
}

/* Consultas invalidas: -1, mensagem preenchida e resultado zerado. */
static void teste_erros(void)
{
    QueryResult r;
    const char *proj_ruim[] = { "id", "idade" };
    printf("- consultas rejeitadas\n");

    CHECK(rodar("SELECT * FROM fantasma;", NULL, 0, &r) == -1);
    CHECK(ex.mensagem[0] != '\0');
    CHECK(r.linhas == NULL && r.numero_linhas == 0);

    CHECK(rodar("SELECT * FROM alunos WHERE id = 'abc';", NULL, 0, &r) == -1);   /* tipo */
    CHECK(rodar("SELECT * FROM alunos WHERE idade = 3;", NULL, 0, &r) == -1);    /* coluna */
    CHECK(rodar("SELECT * FROM alunos;", proj_ruim, 2, &r) == -1);               /* projecao */
    CHECK(strstr(ex.mensagem, "idade") != NULL);

    CHECK(rodar("INSERT INTO alunos VALUES (1, 'Zed');", NULL, 0, &r) == -1);    /* nao e' SELECT */
}

/* Argumentos NULL nao derrubam o executor. */
static void teste_argumentos_nulos(void)
{
    QueryResult r;
    Comando     cmd;
    Parser      parser;
    printf("- argumentos NULL\n");

    iniciar_parser(&parser, "SELECT * FROM alunos;");
    CHECK(parse_comando(&parser, &cmd));

    CHECK(executar_consulta(NULL, &cat, &cmd, NULL, 0, &r) == -1);
    CHECK(executar_consulta(&ex, NULL, &cmd, NULL, 0, &r) == -1);
    CHECK(executar_consulta(&ex, &cat, NULL, NULL, 0, &r) == -1);
    CHECK(executar_consulta(&ex, &cat, &cmd, NULL, 0, NULL) == -1);
}

/* A varredura e' sempre fechada: nenhum pin pendente no cache. */
static void teste_sem_pin_vazado(void)
{
    printf("- nenhum pin vazado\n");
    CHECK(reiniciar_cache() == 0);
}

/* ------------------------------------------------------------------ */

int main(void)
{
    if (montar_ambiente() != 0) {
        fprintf(stderr, "Falha ao montar o ambiente de teste.\n");
        return 2;
    }

    teste_iniciar_executor();
    teste_select_todas_as_linhas();
    teste_where();
    teste_projecao();
    teste_erros();
    teste_argumentos_nulos();
    teste_sem_pin_vazado();      /* por ultimo: depois de todas as consultas */

    remove("alunos.dat");
    printf("\n%d verificacao(oes), %d falha(s).\n", total_testes, falhas);
    return falhas == 0 ? 0 : 1;
}
