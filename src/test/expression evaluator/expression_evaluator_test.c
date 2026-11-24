/* ============================================================
 *            TESTES DO EXPRESSION EVALUATOR
 * ============================================================
 * Compile e execute:
 *   gcc -Wall -Wextra TestExpressionEvaluator.c -o test_evaluator
 *   ./test_evaluator
 *
 * Testa apenas as funcoes do ExpressionEvaluator. O Parser e' usado
 * somente para montar o Comando que avaliar_where recebe.
 */

#include "../../modules/executor/evaluator/expression_evaluator.c"

static int total = 0;
static int falhas = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        total++;                                                         \
        if (!(cond)) {                                                   \
            falhas++;                                                    \
            printf("  FALHOU (linha %d): %s\n", __LINE__, #cond);        \
        }                                                                \
    } while (0)

/* Row montada na mao: alunos(id INT = 30, nome CHAR = "Caio") */
static void montar_row(Row *row)
{
    memset(row, 0, sizeof *row);
    strcpy(row->tabela, "alunos");

    CampoRow *c = &row->campos[row->numero_campos++];
    strcpy(c->nome, "id");
    c->tipo    = TOKEN_INT;
    c->tamanho = (int)sizeof(int32_t);
    c->inteiro = 30;

    c = &row->campos[row->numero_campos++];
    strcpy(c->nome, "nome");
    c->tipo    = TOKEN_CHAR;
    c->tamanho = TAM_CHAR;
    strcpy(c->texto, "Caio");
}

/* Faz o parse de um SELECT e avalia o WHERE. Devolve 1/0, ou -1 se der erro. */
static int where(const Row *row, const char *sql)
{
    Parser    parser;
    Comando   cmd;
    Avaliador av;
    int       resultado;

    iniciar_parser(&parser, sql);
    iniciar_avaliador(&av);
    if (!parse_comando(&parser, &cmd))
        return -2;
    if (avaliar_where(&av, &cmd, row, &resultado) != 0)
        return -1;
    return resultado;
}

/* ---------------------------------------------------------------- */

static void test_construtores_de_value(void)
{
    printf("construtores de Value\n");
    Value i = valor_int(42);
    Value c = valor_char("abc");
    Value b = valor_bool(7);

    CHECK(i.tipo == VALOR_INT  && i.inteiro == 42);
    CHECK(c.tipo == VALOR_CHAR && strcmp(c.texto, "abc") == 0);
    CHECK(b.tipo == VALOR_BOOL && b.booleano == 1);   /* normaliza para 0/1 */
    CHECK(strcmp(nome_tipo_valor(VALOR_INT), "INT") == 0);
}

static void test_literal_e_coluna(void)
{
    printf("literal e coluna\n");
    Row row; montar_row(&row);
    Avaliador av; iniciar_avaliador(&av);
    Value v;

    Expressao lit = expr_literal(valor_int(10));
    CHECK(avaliar_expressao(&av, &lit, &row, &v) == 0);
    CHECK(v.tipo == VALOR_INT && v.inteiro == 10);

    Expressao col_id = expr_coluna("ID");   /* sem diferenciar maiusculas */
    CHECK(avaliar_expressao(&av, &col_id, &row, &v) == 0);
    CHECK(v.tipo == VALOR_INT && v.inteiro == 30);

    Expressao col_nome = expr_coluna("nome");
    CHECK(avaliar_expressao(&av, &col_nome, &row, &v) == 0);
    CHECK(v.tipo == VALOR_CHAR && strcmp(v.texto, "Caio") == 0);
}

static void test_erros_de_coluna(void)
{
    printf("erros de coluna\n");
    Row row; montar_row(&row);
    Avaliador av; iniciar_avaliador(&av);
    Value v;

    Expressao inexistente = expr_coluna("idade");
    CHECK(avaliar_expressao(&av, &inexistente, &row, &v) == -1);
    CHECK(strstr(av.mensagem, "idade") != NULL);

    CHECK(avaliar_expressao(&av, &inexistente, NULL, &v) == -1);   /* sem Row */
}

static void test_comparacao_int(void)
{
    printf("comparacao INT\n");
    Row row; montar_row(&row);
    Avaliador av; iniciar_avaliador(&av);
    Value v;

    Expressao col = expr_coluna("id");
    Expressao dez = expr_literal(valor_int(10));
    Expressao trinta = expr_literal(valor_int(30));

    Expressao maior_igual = expr_comparacao(TOKEN_MAIOR_IGUAL, &col, &dez);
    CHECK(avaliar_expressao(&av, &maior_igual, &row, &v) == 0);
    CHECK(v.tipo == VALOR_BOOL && v.booleano == 1);

    Expressao menor = expr_comparacao(TOKEN_MENOR, &col, &dez);
    CHECK(avaliar_expressao(&av, &menor, &row, &v) == 0);
    CHECK(v.booleano == 0);

    Expressao igual = expr_comparacao(TOKEN_IGUAL, &col, &trinta);
    CHECK(avaliar_expressao(&av, &igual, &row, &v) == 0);
    CHECK(v.booleano == 1);

    Expressao diferente = expr_comparacao(TOKEN_DIFERENTE, &col, &trinta);
    CHECK(avaliar_expressao(&av, &diferente, &row, &v) == 0);
    CHECK(v.booleano == 0);
}

static void test_comparacao_char(void)
{
    printf("comparacao CHAR\n");
    Row row; montar_row(&row);
    Avaliador av; iniciar_avaliador(&av);
    Value v;

    Expressao col  = expr_coluna("nome");
    Expressao caio = expr_literal(valor_char("Caio"));
    Expressao minus = expr_literal(valor_char("caio"));
    Expressao dora = expr_literal(valor_char("Dora"));

    Expressao igual = expr_comparacao(TOKEN_IGUAL, &col, &caio);
    CHECK(avaliar_expressao(&av, &igual, &row, &v) == 0 && v.booleano == 1);

    Expressao sensivel = expr_comparacao(TOKEN_IGUAL, &col, &minus);
    CHECK(avaliar_expressao(&av, &sensivel, &row, &v) == 0 && v.booleano == 0);

    Expressao menor = expr_comparacao(TOKEN_MENOR, &col, &dora);   /* "Caio" < "Dora" */
    CHECK(avaliar_expressao(&av, &menor, &row, &v) == 0 && v.booleano == 1);
}

static void test_erro_de_tipo(void)
{
    printf("erro de tipo\n");
    Row row; montar_row(&row);
    Avaliador av; iniciar_avaliador(&av);
    Value v;

    Expressao col = expr_coluna("nome");
    Expressao dez = expr_literal(valor_int(10));
    Expressao cmp = expr_comparacao(TOKEN_IGUAL, &col, &dez);

    CHECK(avaliar_expressao(&av, &cmp, &row, &v) == -1);
    CHECK(strstr(av.mensagem, "incompativeis") != NULL);
}

static void test_argumentos_invalidos(void)
{
    printf("argumentos invalidos\n");
    Avaliador av; iniciar_avaliador(&av);
    Value v;
    Expressao lit = expr_literal(valor_int(1));
    Expressao sem_filhos = expr_comparacao(TOKEN_IGUAL, NULL, NULL);

    CHECK(avaliar_expressao(&av, NULL, NULL, &v) == -1);
    CHECK(avaliar_expressao(&av, &lit, NULL, NULL) == -1);
    CHECK(avaliar_expressao(&av, &sem_filhos, NULL, &v) == -1);
}

static void test_avaliar_where(void)
{
    printf("avaliar_where\n");
    Row row; montar_row(&row);

    CHECK(where(&row, "SELECT * FROM alunos;") == 1);                                 /* sem WHERE */
    CHECK(where(&row, "SELECT * FROM alunos WHERE id = 30;") == 1);
    CHECK(where(&row, "SELECT * FROM alunos WHERE id > 30;") == 0);
    CHECK(where(&row, "SELECT * FROM alunos WHERE id >= 10 AND nome = 'Caio';") == 1);
    CHECK(where(&row, "SELECT * FROM alunos WHERE id >= 10 AND nome = 'Ana';") == 0);
    CHECK(where(&row, "SELECT * FROM alunos WHERE id = 'abc';") == -1);               /* erro de tipo */
}

int main(void)
{
    test_construtores_de_value();
    test_literal_e_coluna();
    test_erros_de_coluna();
    test_comparacao_int();
    test_comparacao_char();
    test_erro_de_tipo();
    test_argumentos_invalidos();
    test_avaliar_where();

    printf("\n%d verificacoes, %d falha(s)\n", total, falhas);
    return falhas == 0 ? 0 : 1;
}
