/* ============================================================
 *                    TESTES DO FILTER
 * ============================================================
 *
 * Testa apenas as funcoes do Filter: filter_iniciar, filter_abrir,
 * filter_next e filter_fechar (alem do empilhamento Filter sobre Filter).
 *
 * O filho e' um operador SIMULADO (mock) em memoria: nao ha catalogo, disco,
 * cache nem arvore B+. Assim os testes isolam o Filter e permitem forcar
 * falhas do filho (abrir/next) e contar quantas vezes ele foi aberto/fechado.
 *
 * Compilacao (na mesma pasta de Filter.c):
 *   gcc -Wall -Wextra -o filter_test FilterTest.c && ./filter_test
 */

/* TableScan.c e Row.c trazem um main() de demonstracao, desligado por esta macro. */
#define TABLE_SCAN_SEM_MAIN

/* PredicateEvaluator.c define um main() sem protecao: renomeamos so durante
   o include para nao colidir com o main deste arquivo. */
#define main predicate_evaluator_demo_main
#include "../../modules/executor/filter/filter.c"
#undef main

/* ------------------------------------------------------------------ */
/* Mini framework de testes                                            */
/* ------------------------------------------------------------------ */

static int total_testes = 0;
static int total_falhas = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        total_testes++;                                                      \
        if (!(cond)) {                                                       \
            total_falhas++;                                                  \
            printf("    FALHOU (linha %d): %s\n", __LINE__, #cond);          \
        }                                                                    \
    } while (0)

/* ------------------------------------------------------------------ */
/* Operador filho simulado                                             */
/* ------------------------------------------------------------------ */

#define MOCK_MAX 16

typedef struct {
    Row rows[MOCK_MAX];
    int quantidade;
    int posicao;

    int abrir_chamadas;
    int fechar_chamadas;
    int falhar_ao_abrir;     /* 1 = abrir devolve -1                       */
    int falhar_no_next;      /* indice (0-based) em que next devolve -1; -1 = nunca */
} Mock;

static int mock_abrir(void *ctx)
{
    Mock *m = (Mock *)ctx;
    m->abrir_chamadas++;
    if (m->falhar_ao_abrir)
        return -1;
    m->posicao = 0;
    return 0;
}

static int mock_next(void *ctx, Row *saida)
{
    Mock *m = (Mock *)ctx;
    if (m->posicao == m->falhar_no_next)
        return -1;
    if (m->posicao >= m->quantidade)
        return 0;
    *saida = m->rows[m->posicao++];
    return 1;
}

static int mock_fechar(void *ctx)
{
    ((Mock *)ctx)->fechar_chamadas++;
    return 0;
}

static void adicionar_linha(Mock *m, int32_t id, const char *nome)
{
    Row *r = &m->rows[m->quantidade++];
    memset(r, 0, sizeof *r);
    strcpy(r->tabela, "alunos");
    r->numero_campos = 2;

    strcpy(r->campos[0].nome, "id");
    r->campos[0].tipo    = TOKEN_INT;
    r->campos[0].tamanho = (int)sizeof(int32_t);
    r->campos[0].inteiro = id;

    strcpy(r->campos[1].nome, "nome");
    r->campos[1].tipo    = TOKEN_CHAR;
    r->campos[1].tamanho = TAM_CHAR;
    strcpy(r->campos[1].texto, nome);
}

/* Mock com ids 10, 20, 30, 40, 50. */
static void mock_iniciar(Mock *m, Operador *op)
{
    memset(m, 0, sizeof *m);
    m->falhar_no_next = -1;
    adicionar_linha(m, 10, "Ana");
    adicionar_linha(m, 20, "Bia");
    adicionar_linha(m, 30, "Caio");
    adicionar_linha(m, 40, "Davi");
    adicionar_linha(m, 50, "Eva");

    op->contexto = m;
    op->abrir    = mock_abrir;
    op->next     = mock_next;
    op->fechar   = mock_fechar;
}

/* Le o filtro ate o fim e guarda os ids entregues. Devolve o ultimo retorno de next. */
static int drenar(Filter *f, int32_t *ids, int *n)
{
    Row linha;
    int r;
    *n = 0;
    while ((r = filter_next(f, &linha)) == 1) {
        int32_t id = 0;
        row_obter_int(&linha, 0, &id);
        ids[(*n)++] = id;
    }
    return r;
}

/* ------------------------------------------------------------------ */
/* Testes                                                              */
/* ------------------------------------------------------------------ */

static void test_iniciar_validacao(void)
{
    printf("filter_iniciar: argumentos invalidos\n");
    Mock m; Operador op; Avaliador av; Filter f;
    mock_iniciar(&m, &op);
    iniciar_avaliador(&av);

    Operador incompleto = op;
    incompleto.next = NULL;

    CHECK(filter_iniciar(NULL, &op, NULL, &av) == -1);
    CHECK(filter_iniciar(&f, NULL, NULL, &av) == -1);
    CHECK(filter_iniciar(&f, &incompleto, NULL, &av) == -1);
    CHECK(filter_iniciar(&f, &op, NULL, NULL) == -1);
    CHECK(filter_iniciar(&f, &op, NULL, &av) == 0);
    CHECK(m.abrir_chamadas == 0);          /* iniciar nao abre o filho */
}

static void test_uso_fora_de_ordem(void)
{
    printf("filter_abrir / filter_next: uso incorreto\n");
    Mock m; Operador op; Avaliador av; Filter f; Row linha;
    mock_iniciar(&m, &op);
    iniciar_avaliador(&av);
    filter_iniciar(&f, &op, NULL, &av);

    CHECK(filter_next(&f, &linha) == -1);  /* nao aberto */
    CHECK(filter_abrir(&f) == 0);
    CHECK(filter_abrir(&f) == -1);         /* ja aberto */
    CHECK(filter_next(&f, NULL) == -1);
    CHECK(m.abrir_chamadas == 1);
    filter_fechar(&f);
}

static void test_sem_predicado(void)
{
    printf("predicado NULL: todas as tuplas passam\n");
    Mock m; Operador op; Avaliador av; Filter f;
    int32_t ids[MOCK_MAX]; int n;
    mock_iniciar(&m, &op);
    iniciar_avaliador(&av);
    filter_iniciar(&f, &op, NULL, &av);
    filter_abrir(&f);

    CHECK(drenar(&f, ids, &n) == 0);
    CHECK(n == 5);
    CHECK(f.linhas_lidas == 5);
    CHECK(f.linhas_descartadas == 0);
    filter_fechar(&f);
}

static void test_filtra_e_conta(void)
{
    printf("filter_next: entrega so as tuplas que satisfazem o predicado\n");
    Mock m; Operador op; Avaliador av; Filter f;
    int32_t ids[MOCK_MAX]; int n;

    Expressao id  = expr_coluna("id");
    Expressao n20 = expr_literal(valor_int(20));
    Expressao n40 = expr_literal(valor_int(40));
    Predicado ge  = pred_comparacao(TOKEN_MAIOR_IGUAL, &id, &n20);
    Predicado lt  = pred_comparacao(TOKEN_MENOR, &id, &n40);
    Predicado faixa = pred_and(&ge, &lt);       /* 20 <= id < 40 */

    mock_iniciar(&m, &op);
    iniciar_avaliador(&av);
    filter_iniciar(&f, &op, &faixa, &av);
    filter_abrir(&f);

    CHECK(drenar(&f, ids, &n) == 0);
    CHECK(n == 2);
    CHECK(n == 2 && ids[0] == 20 && ids[1] == 30);
    CHECK(f.linhas_lidas == 5);
    CHECK(f.linhas_descartadas == 3);
    filter_fechar(&f);
}

static void test_nenhuma_satisfaz(void)
{
    printf("filter_next: nenhuma tupla satisfaz; fim e' estavel\n");
    Mock m; Operador op; Avaliador av; Filter f; Row linha;

    Expressao id    = expr_coluna("id");
    Expressao n1000 = expr_literal(valor_int(1000));
    Predicado nada  = pred_comparacao(TOKEN_MAIOR, &id, &n1000);

    mock_iniciar(&m, &op);
    iniciar_avaliador(&av);
    filter_iniciar(&f, &op, &nada, &av);
    filter_abrir(&f);

    CHECK(filter_next(&f, &linha) == 0);
    CHECK(f.linhas_descartadas == 5);
    CHECK(filter_next(&f, &linha) == 0);   /* continua devolvendo 0 */
    filter_fechar(&f);
}

static void test_erro_de_avaliacao(void)
{
    printf("filter_next: erro de avaliacao vira -1 (nunca 'falso')\n");
    Mock m; Operador op; Avaliador av; Filter f; Row linha;

    Expressao idade = expr_coluna("idade");     /* coluna inexistente */
    Expressao n30   = expr_literal(valor_int(30));
    Predicado ruim  = pred_comparacao(TOKEN_IGUAL, &idade, &n30);

    mock_iniciar(&m, &op);
    iniciar_avaliador(&av);
    filter_iniciar(&f, &op, &ruim, &av);
    filter_abrir(&f);

    CHECK(filter_next(&f, &linha) == -1);
    CHECK(strlen(av.mensagem) > 0);
    CHECK(filter_next(&f, &linha) == 0);   /* varredura encerrada */
    CHECK(filter_fechar(&f) == 0);
    CHECK(m.fechar_chamadas == 1);         /* filho liberado apos o erro */
}

static void test_falhas_do_filho(void)
{
    printf("falhas do operador filho\n");
    Mock m; Operador op; Avaliador av; Filter f; Row linha;

    /* a) abrir do filho falha: Filter nao fica aberto e nao fecha o filho */
    mock_iniciar(&m, &op);
    m.falhar_ao_abrir = 1;
    iniciar_avaliador(&av);
    filter_iniciar(&f, &op, NULL, &av);
    CHECK(filter_abrir(&f) == -1);
    CHECK(filter_next(&f, &linha) == -1);
    CHECK(filter_fechar(&f) == 0);
    CHECK(m.fechar_chamadas == 0);

    /* b) next do filho falha na 3a tupla: devolve 2 tuplas e depois -1 */
    mock_iniciar(&m, &op);
    m.falhar_no_next = 2;
    filter_iniciar(&f, &op, NULL, &av);
    filter_abrir(&f);
    CHECK(filter_next(&f, &linha) == 1);
    CHECK(filter_next(&f, &linha) == 1);
    CHECK(filter_next(&f, &linha) == -1);
    filter_fechar(&f);
}

static void test_fechar_e_reabrir(void)
{
    printf("filter_fechar: idempotente, fecha o filho e permite reabrir\n");
    Mock m; Operador op; Avaliador av; Filter f; Row linha;
    int32_t ids[MOCK_MAX]; int n;

    mock_iniciar(&m, &op);
    iniciar_avaliador(&av);
    filter_iniciar(&f, &op, NULL, &av);
    filter_abrir(&f);
    filter_next(&f, &linha);               /* desiste no meio */

    CHECK(filter_fechar(&f) == 0);
    CHECK(m.fechar_chamadas == 1);
    CHECK(filter_fechar(&f) == 0);         /* segunda chamada e' segura */
    CHECK(m.fechar_chamadas == 1);         /* e nao fecha o filho de novo */
    CHECK(filter_fechar(NULL) == -1);

    CHECK(filter_abrir(&f) == 0);          /* reabre do comeco */
    CHECK(drenar(&f, ids, &n) == 0);
    CHECK(n == 5);
    CHECK(f.linhas_lidas == 5);            /* contadores zerados na reabertura */
    filter_fechar(&f);
}

static void test_filter_sobre_filter(void)
{
    printf("Filter sobre Filter (operador_de_filter)\n");
    Mock m; Operador op, interno_op; Avaliador av1, av2; Filter interno, externo;
    int32_t ids[MOCK_MAX]; int n;

    Expressao id  = expr_coluna("id");
    Expressao n20 = expr_literal(valor_int(20));
    Expressao n40 = expr_literal(valor_int(40));
    Predicado ge  = pred_comparacao(TOKEN_MAIOR_IGUAL, &id, &n20);
    Predicado lt  = pred_comparacao(TOKEN_MENOR, &id, &n40);

    mock_iniciar(&m, &op);
    iniciar_avaliador(&av1);
    iniciar_avaliador(&av2);
    filter_iniciar(&interno, &op, &ge, &av1);
    operador_de_filter(&interno_op, &interno);
    filter_iniciar(&externo, &interno_op, &lt, &av2);

    CHECK(filter_abrir(&externo) == 0);
    CHECK(drenar(&externo, ids, &n) == 0);
    CHECK(n == 2 && ids[0] == 20 && ids[1] == 30);
    CHECK(filter_fechar(&externo) == 0);
    CHECK(m.fechar_chamadas == 1);         /* o fechamento desce ate o filho */
}

/* ------------------------------------------------------------------ */

int main(void)
{
    test_iniciar_validacao();
    test_uso_fora_de_ordem();
    test_sem_predicado();
    test_filtra_e_conta();
    test_nenhuma_satisfaz();
    test_erro_de_avaliacao();
    test_falhas_do_filho();
    test_fechar_e_reabrir();
    test_filter_sobre_filter();

    printf("\n%d verificacoes, %d falha(s)\n", total_testes, total_falhas);
    return total_falhas == 0 ? 0 : 1;
}
