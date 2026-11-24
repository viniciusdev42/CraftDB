/* ============================================================
 *            TESTES DO PREDICATE EVALUATOR
 * ============================================================
 *
 * Compilar (parser.c, row.c, expression_evaluator.c e predicate_evaluator.c
 * devem estar acessiveis no include path / mesmo diretorio):
 *
 *   gcc -Wall -o test_pred test_predicate_evaluator.c && ./test_pred
 *
 * O predicate_evaluator.c define um main() de demonstracao. Para nao conflitar
 * com o main() deste arquivo, ele e' renomeado durante o #include.
 */
#include "../../modules/executor/evaluator/predicate_evaluator.c"

static int total  = 0;
static int falhas = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        total++;                                                           \
        if (!(cond)) {                                                     \
            falhas++;                                                      \
            printf("  FALHOU (linha %d): %s\n", __LINE__, #cond);          \
        }                                                                  \
    } while (0)

/* ------------------------------------------------------------------ */
/* Auxiliares                                                          */
/* ------------------------------------------------------------------ */

static void add_campo(Row *row, const char *nome, TipoToken tipo, int32_t inteiro, const char *texto)
{
    CampoRow *c = &row->campos[row->numero_campos++];
    strcpy(c->nome, nome);
    c->tipo = tipo;
    if (tipo == TOKEN_INT) {
        c->tamanho = (int)sizeof(int32_t);
        c->inteiro = inteiro;
    } else {
        c->tamanho = TAM_CHAR;
        strcpy(c->texto, texto);
    }
}

/* Avalia e devolve: 1 / 0 = resultado do predicado, -1 = erro. */
static int avaliar(const Predicado *p, const Row *row, Avaliador *av)
{
    int r = -99;
    iniciar_avaliador(av);
    if (avaliar_predicado(av, p, row, &r) != 0)
        return -1;
    return r;
}

/* Avalia o WHERE de um SQL: 1 / 0 = permanece ou nao, -1 = erro. */
static int where(const Row *row, const char *sql)
{
    Parser    parser;
    Comando   cmd;
    Avaliador av;
    int       r = -99;

    iniciar_parser(&parser, sql);
    iniciar_avaliador(&av);
    if (!parse_comando(&parser, &cmd))
        return -2;                          /* erro de sintaxe */
    if (linha_permanece_where(&av, &cmd, row, &r) != 0)
        return -1;
    return r;
}

/* ------------------------------------------------------------------ */
/* Testes                                                              */
/* ------------------------------------------------------------------ */

static void teste_comparacao(const Row *row)
{
    Avaliador av;
    Expressao id   = expr_coluna("ID");         /* sem case */
    Expressao nome = expr_coluna("nome");
    Expressao n10  = expr_literal(valor_int(10));
    Expressao n30  = expr_literal(valor_int(30));
    Expressao caio = expr_literal(valor_char("Caio"));
    Expressao dora = expr_literal(valor_char("Dora"));

    printf("teste_comparacao\n");

    Predicado eq = pred_comparacao(TOKEN_IGUAL,       &id, &n30);
    Predicado ne = pred_comparacao(TOKEN_DIFERENTE,   &id, &n30);
    Predicado lt = pred_comparacao(TOKEN_MENOR,       &id, &n10);
    Predicado le = pred_comparacao(TOKEN_MENOR_IGUAL, &id, &n30);
    Predicado gt = pred_comparacao(TOKEN_MAIOR,       &id, &n10);
    Predicado ge = pred_comparacao(TOKEN_MAIOR_IGUAL, &id, &n30);

    CHECK(avaliar(&eq, row, &av) == 1);
    CHECK(avaliar(&ne, row, &av) == 0);
    CHECK(avaliar(&lt, row, &av) == 0);
    CHECK(avaliar(&le, row, &av) == 1);
    CHECK(avaliar(&gt, row, &av) == 1);
    CHECK(avaliar(&ge, row, &av) == 1);

    /* CHAR */
    Predicado nome_eq = pred_comparacao(TOKEN_IGUAL, &nome, &caio);
    Predicado nome_lt = pred_comparacao(TOKEN_MENOR, &nome, &dora);
    Predicado nome_gt = pred_comparacao(TOKEN_MAIOR, &nome, &dora);

    CHECK(avaliar(&nome_eq, row, &av) == 1);
    CHECK(avaliar(&nome_lt, row, &av) == 1);
    CHECK(avaliar(&nome_gt, row, &av) == 0);
}

static void teste_constante_e_logicos(const Row *row)
{
    Avaliador av;
    Expressao id   = expr_coluna("id");
    Expressao n10  = expr_literal(valor_int(10));
    Expressao n30  = expr_literal(valor_int(30));

    printf("teste_constante_e_logicos\n");

    Predicado V = pred_constante(1);
    Predicado F = pred_constante(0);
    Predicado verdadeiro_qualquer = pred_constante(42);     /* normaliza para 1 */

    CHECK(avaliar(&V, row, &av) == 1);
    CHECK(avaliar(&F, row, &av) == 0);
    CHECK(avaliar(&verdadeiro_qualquer, row, &av) == 1);

    Predicado ge30 = pred_comparacao(TOKEN_MAIOR_IGUAL, &id, &n30);   /* verdadeiro */
    Predicado lt10 = pred_comparacao(TOKEN_MENOR,       &id, &n10);   /* falso      */

    Predicado e1 = pred_and(&ge30, &ge30);
    Predicado e2 = pred_and(&ge30, &lt10);
    Predicado o1 = pred_or(&lt10, &ge30);
    Predicado o2 = pred_or(&lt10, &lt10);
    Predicado n1 = pred_not(&lt10);
    Predicado n2 = pred_not(&ge30);

    CHECK(avaliar(&e1, row, &av) == 1);
    CHECK(avaliar(&e2, row, &av) == 0);
    CHECK(avaliar(&o1, row, &av) == 1);
    CHECK(avaliar(&o2, row, &av) == 0);
    CHECK(avaliar(&n1, row, &av) == 1);
    CHECK(avaliar(&n2, row, &av) == 0);

    /* composicao: (id>=30 AND id<10) OR NOT(id<10) -> falso OR verdadeiro */
    Predicado mista = pred_or(&e2, &n1);
    CHECK(avaliar(&mista, row, &av) == 1);
}

static void teste_curto_circuito(const Row *row)
{
    Avaliador av;
    Expressao id    = expr_coluna("id");
    Expressao idade = expr_coluna("idade");     /* coluna inexistente */
    Expressao n10   = expr_literal(valor_int(10));
    Expressao n30   = expr_literal(valor_int(30));

    printf("teste_curto_circuito\n");

    Predicado ge30  = pred_comparacao(TOKEN_MAIOR_IGUAL, &id,    &n30);   /* verdadeiro */
    Predicado lt10  = pred_comparacao(TOKEN_MENOR,       &id,    &n10);   /* falso      */
    Predicado ruim  = pred_comparacao(TOKEN_IGUAL,       &idade, &n10);   /* erro       */

    /* O lado direito (com erro) nao deve ser avaliado. */
    Predicado and_curto = pred_and(&lt10, &ruim);
    Predicado or_curto  = pred_or(&ge30, &ruim);
    CHECK(avaliar(&and_curto, row, &av) == 0);
    CHECK(avaliar(&or_curto,  row, &av) == 1);

    /* Quando o lado direito precisa ser avaliado, o erro aparece. */
    Predicado and_forca = pred_and(&ge30, &ruim);
    Predicado or_forca  = pred_or(&lt10, &ruim);
    CHECK(avaliar(&and_forca, row, &av) == -1);
    CHECK(strlen(av.mensagem) > 0);
    CHECK(avaliar(&or_forca, row, &av) == -1);
}

static void teste_erros(const Row *row)
{
    Avaliador av;
    int       r;
    Expressao id   = expr_coluna("id");
    Expressao nome = expr_coluna("nome");
    Expressao n10  = expr_literal(valor_int(10));
    Expressao n30  = expr_literal(valor_int(30));

    printf("teste_erros\n");

    /* tipos incompativeis (CHAR x INT) */
    Predicado tipos = pred_comparacao(TOKEN_IGUAL, &nome, &n10);
    CHECK(avaliar(&tipos, row, &av) == -1);
    CHECK(strlen(av.mensagem) > 0);

    /* operador invalido */
    Predicado op_ruim = pred_comparacao(TOKEN_INT, &id, &n10);
    CHECK(avaliar(&op_ruim, row, &av) == -1);

    /* operando ausente na comparacao */
    Predicado sem_op = pred_comparacao(TOKEN_IGUAL, &id, NULL);
    CHECK(avaliar(&sem_op, row, &av) == -1);

    /* operandos ausentes em AND / OR / NOT */
    Predicado ge30   = pred_comparacao(TOKEN_MAIOR_IGUAL, &id, &n30);
    Predicado and_nu = pred_and(&ge30, NULL);
    Predicado or_nu  = pred_or(NULL, &ge30);
    Predicado not_nu = pred_not(NULL);
    CHECK(avaliar(&and_nu, row, &av) == -1);
    CHECK(avaliar(&or_nu,  row, &av) == -1);
    CHECK(avaliar(&not_nu, row, &av) == -1);

    /* predicado NULL em avaliar_predicado e' erro */
    CHECK(avaliar(NULL, row, &av) == -1);

    /* tipo de predicado desconhecido */
    Predicado desconhecido = pred_constante(1);
    desconhecido.tipo = (TipoPredicado)99;
    CHECK(avaliar(&desconhecido, row, &av) == -1);

    /* resultado NULL */
    iniciar_avaliador(&av);
    CHECK(avaliar_predicado(&av, &ge30, row, NULL) == -1);

    /* avaliador NULL */
    CHECK(avaliar_predicado(NULL, &ge30, row, &r) == -1);
}

static void teste_profundidade(const Row *row)
{
    Avaliador av;
    Predicado cadeia[MAX_PROFUNDIDADE_PREDICADO + 10];
    int       n = (int)(sizeof cadeia / sizeof cadeia[0]);

    printf("teste_profundidade\n");

    /* NOT(NOT(NOT(... TRUE))) com mais niveis que o limite */
    cadeia[0] = pred_constante(1);
    for (int i = 1; i < n; i++)
        cadeia[i] = pred_not(&cadeia[i - 1]);

    CHECK(avaliar(&cadeia[n - 1], row, &av) == -1);     /* funda demais */
    CHECK(avaliar(&cadeia[5], row, &av) == 0);          /* 5 NOTs de TRUE = 0 */
}

static void teste_linha_permanece(const Row *row)
{
    Avaliador av;
    int       permanece = -99;
    Expressao id  = expr_coluna("id");
    Expressao n10 = expr_literal(valor_int(10));

    printf("teste_linha_permanece\n");

    /* sem predicado: a linha sempre permanece */
    iniciar_avaliador(&av);
    CHECK(linha_permanece(&av, NULL, row, &permanece) == 0 && permanece == 1);

    /* com predicado */
    Predicado gt10 = pred_comparacao(TOKEN_MAIOR, &id, &n10);
    Predicado lt10 = pred_comparacao(TOKEN_MENOR, &id, &n10);
    CHECK(linha_permanece(&av, &gt10, row, &permanece) == 0 && permanece == 1);
    CHECK(linha_permanece(&av, &lt10, row, &permanece) == 0 && permanece == 0);

    /* permanece NULL e avaliador NULL */
    CHECK(linha_permanece(&av, &gt10, row, NULL) == -1);
    CHECK(linha_permanece(NULL, &gt10, row, &permanece) == -1);
}

static void teste_where(const Row *row)
{
    printf("teste_where\n");

    CHECK(where(row, "SELECT * FROM alunos;") == 1);                                   /* sem WHERE */
    CHECK(where(row, "SELECT * FROM alunos WHERE id = 30;") == 1);
    CHECK(where(row, "SELECT * FROM alunos WHERE id > 30;") == 0);
    CHECK(where(row, "SELECT * FROM alunos WHERE id >= 10 AND nome = 'Caio';") == 1);
    CHECK(where(row, "SELECT * FROM alunos WHERE id >= 10 AND nome = 'Dora';") == 0);
    CHECK(where(row, "SELECT * FROM alunos WHERE id = 'abc';") == -1);                /* tipos */
    CHECK(where(row, "SELECT * FROM alunos WHERE idade = 1;") == -1);                  /* coluna */

    /* curto-circuito: a 1a condicao falsa evita o erro da 2a */
    CHECK(where(row, "SELECT * FROM alunos WHERE id > 30 AND idade = 1;") == 0);

    /* argumentos NULL */
    Avaliador av;
    int       r;
    iniciar_avaliador(&av);
    CHECK(linha_permanece_where(&av, NULL, row, &r) == -1);
}

/* ------------------------------------------------------------------ */

int main(void)
{
    Row row;

    memset(&row, 0, sizeof row);
    strcpy(row.tabela, "alunos");
    add_campo(&row, "id",   TOKEN_INT,  30, NULL);
    add_campo(&row, "nome", TOKEN_CHAR, 0,  "Caio");

    teste_comparacao(&row);
    teste_constante_e_logicos(&row);
    teste_curto_circuito(&row);
    teste_erros(&row);
    teste_profundidade(&row);
    teste_linha_permanece(&row);
    teste_where(&row);

    printf("\n%d verificacoes, %d falha(s)\n", total, falhas);
    return falhas == 0 ? 0 : 1;
}
