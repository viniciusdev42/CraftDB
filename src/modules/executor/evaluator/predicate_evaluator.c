/* ============================================================
 *                 PREDICATE EVALUATOR
 * ============================================================
 *
 * Avalia PREDICADOS (expressoes booleanas) sobre uma row e decide se a linha
 * deve permanecer no resultado de um SELECT.
 *
 * Camadas (cada uma so depende da anterior):
 *
 *   1) Comparacao : =, <>, <, <=, >, >=. Delegada ao ExpressionEvaluator
 *                   (avaliar_expressao), que resolve colunas, literais e tipos.
 *   2) Logicos    : AND, OR, NOT, compostos livremente (arvore de predicados).
 *   3) Linha      : linha_permanece(), que aplica um predicado (ou o WHERE de
 *                   um Comando do parser) e diz se a linha fica ou sai.
 *
 * Expressoes booleanas NAO suportadas nunca passam em silencio: viram erro com
 * mensagem em Avaliador.mensagem, em vez de serem tratadas como "falso" ou
 * "verdadeiro". Sao rejeitados:
 *   - operador de comparacao invalido (ex.: um token que nao seja =, <>, <...);
 *   - no de comparacao malformado ou com operando ausente;
 *   - comparacao entre tipos incompativeis (INT x CHAR, sem conversao);
 *   - expressao folha cujo resultado nao seja BOOL;
 *   - tipo de predicado desconhecido;
 *   - arvore com ponteiro NULL, ou funda demais (protege contra ciclos).
 *
 * Curto-circuito: em "A AND B", se A e' falso, B nao e' avaliado; em "A OR B",
 * se A e' verdadeiro, B nao e' avaliado. Logo, um erro dentro de B so aparece
 * quando B realmente precisa ser avaliado.
 *
 * Convencao de retorno (igual a do ExpressionEvaluator): 0 = sucesso, -1 = erro.
 * Ciclo de vida: o chamador e' dono dos nos (sem malloc); os filhos de um
 * predicado sao ponteiros e devem viver ate o fim da avaliacao.
 */

#ifndef PREDICATE_EVALUATOR
#define PREDICATE_EVALUATOR

#define main EXPRESSION_EVALUATOR_SEM_MAIN
#include "expression_evaluator.c"
#undef main 

/* Profundidade maxima da arvore; evita estouro de pilha e ciclos acidentais. */
#define MAX_PROFUNDIDADE_PREDICADO 64

/* ------------------------------------------------------------------ */
/* Nos do predicado                                                    */
/* ------------------------------------------------------------------ */

typedef enum {
    PRED_CONSTANTE,     /* TRUE / FALSE literal                       */
    PRED_COMPARACAO,    /* coluna/literal <op> coluna/literal         */
    PRED_AND,           /* esquerda AND direita                       */
    PRED_OR,            /* esquerda OR  direita                       */
    PRED_NOT            /* NOT esquerda                               */
} TipoPredicado;

typedef struct Predicado Predicado;
struct Predicado {
    TipoPredicado tipo;

    int           constante;        /* PRED_CONSTANTE: 0 ou 1                         */
    Expressao     comparacao;       /* PRED_COMPARACAO: raiz EXPR_COMPARACAO          */

    const Predicado *esquerda;      /* PRED_AND / PRED_OR / PRED_NOT (operando unico) */
    const Predicado *direita;       /* PRED_AND / PRED_OR                             */
};

static Predicado pred_base(TipoPredicado tipo)
{
    Predicado p;
    memset(&p, 0, sizeof p);
    p.tipo = tipo;
    return p;
}

static Predicado pred_constante(int valor)
{
    Predicado p = pred_base(PRED_CONSTANTE);
    p.constante = valor ? 1 : 0;
    return p;
}

/* Os operandos esq/dir sao Expressao (coluna ou literal) mantidas pelo chamador. */
static Predicado pred_comparacao(TipoToken operador, const Expressao *esq, const Expressao *dir)
{
    Predicado p = pred_base(PRED_COMPARACAO);
    p.comparacao = expr_comparacao(operador, esq, dir);
    return p;
}

static Predicado pred_and(const Predicado *esq, const Predicado *dir)
{
    Predicado p = pred_base(PRED_AND);
    p.esquerda = esq;
    p.direita  = dir;
    return p;
}

static Predicado pred_or(const Predicado *esq, const Predicado *dir)
{
    Predicado p = pred_base(PRED_OR);
    p.esquerda = esq;
    p.direita  = dir;
    return p;
}

static Predicado pred_not(const Predicado *operando)
{
    Predicado p = pred_base(PRED_NOT);
    p.esquerda = operando;
    return p;
}

/* ------------------------------------------------------------------ */
/* Etapa 1: operadores de comparacao                                   */
/* ------------------------------------------------------------------ */

static int operador_comparacao_valido(TipoToken op)
{
    switch (op) {
        case TOKEN_IGUAL:
        case TOKEN_DIFERENTE:
        case TOKEN_MENOR:
        case TOKEN_MENOR_IGUAL:
        case TOKEN_MAIOR:
        case TOKEN_MAIOR_IGUAL:
            return 1;
        default:
            return 0;
    }
}

/* Valida o no e avalia a comparacao pelo ExpressionEvaluator. O resultado deve
   ser BOOL; qualquer outra coisa e' uma expressao booleana nao suportada. */
static int pred_avaliar_comparacao(Avaliador *av, const Predicado *p, const Row *row,
                                   int *resultado)
{
    const Expressao *cmp = &p->comparacao;
    Value v;

    if (cmp->tipo != EXPR_COMPARACAO)
        return av_erro(av, "predicado de comparacao malformado: no nao e' uma comparacao");
    if (!operador_comparacao_valido(cmp->operador))
        return av_erro(av, "operador de comparacao nao suportado em predicado");
    if (cmp->esquerda == NULL || cmp->direita == NULL)
        return av_erro(av, "comparacao com operando ausente");

    if (avaliar_expressao(av, cmp, row, &v) != 0)
        return -1;
    if (v.tipo != VALOR_BOOL)
        return av_erro(av, "expressao nao booleana em predicado: resultado %s",
                       nome_tipo_valor(v.tipo));

    *resultado = v.booleano;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Etapa 2: operadores logicos                                         */
/* ------------------------------------------------------------------ */

static int pred_avaliar(Avaliador *av, const Predicado *p, const Row *row,
                        int profundidade, int *resultado);

/* Avalia um operando exigindo que exista. */
static int pred_avaliar_operando(Avaliador *av, const Predicado *operando, const Row *row,
                                 int profundidade, const char *nome_op, int *resultado)
{
    if (operando == NULL)
        return av_erro(av, "operador %s com operando ausente", nome_op);
    return pred_avaliar(av, operando, row, profundidade + 1, resultado);
}

static int pred_avaliar_and(Avaliador *av, const Predicado *p, const Row *row,
                            int profundidade, int *resultado)
{
    int esq, dir;

    if (pred_avaliar_operando(av, p->esquerda, row, profundidade, "AND", &esq) != 0)
        return -1;
    if (!esq) {                         /* curto-circuito */
        *resultado = 0;
        return 0;
    }
    if (pred_avaliar_operando(av, p->direita, row, profundidade, "AND", &dir) != 0)
        return -1;
    *resultado = dir;
    return 0;
}

static int pred_avaliar_or(Avaliador *av, const Predicado *p, const Row *row,
                           int profundidade, int *resultado)
{
    int esq, dir;

    if (pred_avaliar_operando(av, p->esquerda, row, profundidade, "OR", &esq) != 0)
        return -1;
    if (esq) {                          /* curto-circuito */
        *resultado = 1;
        return 0;
    }
    if (pred_avaliar_operando(av, p->direita, row, profundidade, "OR", &dir) != 0)
        return -1;
    *resultado = dir;
    return 0;
}

static int pred_avaliar_not(Avaliador *av, const Predicado *p, const Row *row,
                            int profundidade, int *resultado)
{
    int operando;

    if (pred_avaliar_operando(av, p->esquerda, row, profundidade, "NOT", &operando) != 0)
        return -1;
    *resultado = !operando;
    return 0;
}

static int pred_avaliar(Avaliador *av, const Predicado *p, const Row *row,
                        int profundidade, int *resultado)
{
    if (p == NULL)
        return av_erro(av, "predicado NULL");
    if (profundidade > MAX_PROFUNDIDADE_PREDICADO)
        return av_erro(av, "predicado profundo demais (limite %d)", MAX_PROFUNDIDADE_PREDICADO);

    switch (p->tipo) {
        case PRED_CONSTANTE:
            *resultado = p->constante ? 1 : 0;
            return 0;
        case PRED_COMPARACAO:
            return pred_avaliar_comparacao(av, p, row, resultado);
        case PRED_AND:
            return pred_avaliar_and(av, p, row, profundidade, resultado);
        case PRED_OR:
            return pred_avaliar_or(av, p, row, profundidade, resultado);
        case PRED_NOT:
            return pred_avaliar_not(av, p, row, profundidade, resultado);
    }
    return av_erro(av, "tipo de predicado desconhecido");
}

/*
 avaliar_predicado(av, predicado, row, resultado)
 Avalia a arvore de predicados contra a row. *resultado recebe 0 ou 1.
 'row' pode ser NULL apenas se o predicado nao referenciar colunas.
 Retorna 0 em sucesso, -1 em erro (mensagem em av->mensagem).
*/
static int avaliar_predicado(Avaliador *av, const Predicado *predicado, const Row *row,
                             int *resultado)
{
    if (av == NULL)
        return -1;
    if (resultado == NULL)
        return av_erro(av, "avaliar_predicado recebeu resultado NULL");
    return pred_avaliar(av, predicado, row, 0, resultado);
}

/* ------------------------------------------------------------------ */
/* Etapa 3: a linha permanece no resultado?                            */
/* ------------------------------------------------------------------ */

/*
 linha_permanece(av, predicado, row, permanece)
 Predicado NULL significa "sem filtro": a linha sempre permanece.
 Retorna 0 em sucesso, -1 em erro; em erro a linha NAO deve ser incluida.
*/
static int linha_permanece(Avaliador *av, const Predicado *predicado, const Row *row,
                           int *permanece)
{
    if (av == NULL)
        return -1;
    if (permanece == NULL)
        return av_erro(av, "linha_permanece recebeu argumento NULL");
    if (predicado == NULL) {
        *permanece = 1;
        return 0;
    }
    return avaliar_predicado(av, predicado, row, permanece);
}

/* ------------------------------------------------------------------ */
/* Adaptador: Condicao do Parser -> Predicado                          */
/* ------------------------------------------------------------------ */

/* Agrupa a Condicao e seu predicado para que o chamador mantenha os nos vivos. */
typedef struct {
    ExprCondicao arvore;
    Predicado    predicado;
} PredCondicao;

static void pred_de_condicao(PredCondicao *dest, const Condicao *c)
{
    expr_de_condicao(&dest->arvore, c);
    dest->predicado = pred_comparacao(c->operacao, &dest->arvore.coluna, &dest->arvore.literal);
}

/*
 linha_permanece_where(av, comando, row, permanece)
 Aplica o WHERE de um SELECT (condicoes ligadas por AND, com curto-circuito).
 Sem condicoes, a linha permanece.
*/
static int linha_permanece_where(Avaliador *av, const Comando *comando, const Row *row,
                                 int *permanece)
{
    if (av == NULL)
        return -1;
    if (comando == NULL || permanece == NULL)
        return av_erro(av, "linha_permanece_where recebeu argumento NULL");

    *permanece = 1;
    for (int i = 0; i < comando->numero_condicoes; i++) {
        PredCondicao pc;
        int          ok;

        pred_de_condicao(&pc, &comando->condicoes[i]);
        if (avaliar_predicado(av, &pc.predicado, row, &ok) != 0)
            return -1;
        if (!ok) {
            *permanece = 0;
            return 0;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Demonstracao                                                        */
/* ------------------------------------------------------------------ */

/* Monta a row na mao: o avaliador nao precisa de catalogo nem de disco. */
static void demo_pred_campo(Row *row, const char *nome, TipoToken tipo, int32_t inteiro,
                            const char *texto)
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

static void demo_pred(const char *rotulo, const Predicado *p, const Row *row)
{
    Avaliador av;
    int       permanece;

    iniciar_avaliador(&av);
    if (linha_permanece(&av, p, row, &permanece) != 0)
        printf("%-44s -> erro: %s\n", rotulo, av.mensagem);
    else
        printf("%-44s -> %s\n", rotulo, permanece ? "permanece" : "descartada");
}

static void demo_pred_where(const Row *row, const char *sql)
{
    Parser    parser;
    Comando   cmd;
    Avaliador av;
    int       permanece;

    iniciar_parser(&parser, sql);
    iniciar_avaliador(&av);
    if (!parse_comando(&parser, &cmd)) {
        printf("%-58s -> erro de sintaxe: %s\n", sql, parser.mensagem);
        return;
    }
    if (linha_permanece_where(&av, &cmd, row, &permanece) != 0)
        printf("%-58s -> erro: %s\n", sql, av.mensagem);
    else
        printf("%-58s -> %s\n", sql, permanece ? "permanece" : "descartada");
}
#ifdef PREDICATE_EVALUATOR_SEM_MAIN
int main(void)
{
    Row row;

    memset(&row, 0, sizeof row);
    strcpy(row.tabela, "alunos");
    demo_pred_campo(&row, "id",   TOKEN_INT,  30, NULL);
    demo_pred_campo(&row, "nome", TOKEN_CHAR, 0,  "Caio");

    /* Operandos reutilizados pelos predicados abaixo. */
    Expressao id    = expr_coluna("ID");                /* sem case */
    Expressao nome  = expr_coluna("nome");
    Expressao idade = expr_coluna("idade");             /* coluna inexistente */
    Expressao n10   = expr_literal(valor_int(10));
    Expressao n30   = expr_literal(valor_int(30));
    Expressao caio  = expr_literal(valor_char("Caio"));
    Expressao dora  = expr_literal(valor_char("Dora"));

    /* 1) Operadores de comparacao */
    Predicado c_eq = pred_comparacao(TOKEN_IGUAL,       &id,   &n30);
    Predicado c_ne = pred_comparacao(TOKEN_DIFERENTE,   &id,   &n30);
    Predicado c_lt = pred_comparacao(TOKEN_MENOR,       &id,   &n10);
    Predicado c_le = pred_comparacao(TOKEN_MENOR_IGUAL, &id,   &n30);
    Predicado c_gt = pred_comparacao(TOKEN_MAIOR,       &id,   &n10);
    Predicado c_ge = pred_comparacao(TOKEN_MAIOR_IGUAL, &id,   &n30);
    Predicado c_nm = pred_comparacao(TOKEN_MENOR,       &nome, &dora);
    Predicado c_ok = pred_comparacao(TOKEN_IGUAL,       &nome, &caio);

    printf("-- comparacao (id=30, nome='Caio')\n");
    demo_pred("id = 30",         &c_eq, &row);
    demo_pred("id <> 30",        &c_ne, &row);
    demo_pred("id < 10",         &c_lt, &row);
    demo_pred("id <= 30",        &c_le, &row);
    demo_pred("id > 10",         &c_gt, &row);
    demo_pred("id >= 30",        &c_ge, &row);
    demo_pred("nome < 'Dora'",   &c_nm, &row);

    /* 2) Operadores logicos */
    Predicado verdadeiro = pred_constante(1);
    Predicado falso      = pred_constante(0);

    Predicado e_ok   = pred_and(&c_ge, &c_ok);              /* id>=30 AND nome='Caio' */
    Predicado e_no   = pred_and(&c_ge, &c_lt);              /* id>=30 AND id<10       */
    Predicado ou_ok  = pred_or(&c_lt, &c_ok);               /* id<10 OR nome='Caio'   */
    Predicado ou_no  = pred_or(&c_lt, &c_ne);               /* id<10 OR id<>30        */
    Predicado nao_ok = pred_not(&c_lt);                     /* NOT id<10              */
    Predicado nao_no = pred_not(&c_eq);                     /* NOT id=30              */
    Predicado nao_ou = pred_not(&ou_no);                    /* NOT (id<10 OR id<>30)  */
    Predicado mista  = pred_or(&e_no, &nao_ou);             /* (A AND B) OR NOT(...)  */

    printf("\n-- logicos\n");
    demo_pred("TRUE",                              &verdadeiro, &row);
    demo_pred("FALSE",                             &falso,      &row);
    demo_pred("id>=30 AND nome='Caio'",            &e_ok,       &row);
    demo_pred("id>=30 AND id<10",                  &e_no,       &row);
    demo_pred("id<10 OR nome='Caio'",              &ou_ok,      &row);
    demo_pred("id<10 OR id<>30",                   &ou_no,      &row);
    demo_pred("NOT id<10",                         &nao_ok,     &row);
    demo_pred("NOT id=30",                         &nao_no,     &row);
    demo_pred("(id>=30 AND id<10) OR NOT(id<10 OR id<>30)", &mista, &row);
    demo_pred("(sem predicado)",                   NULL,        &row);

    /* 3) Curto-circuito e expressoes nao suportadas */
    Predicado c_idade   = pred_comparacao(TOKEN_IGUAL, &idade, &n10);
    Predicado c_tipo    = pred_comparacao(TOKEN_IGUAL, &nome,  &n10);
    Predicado c_op_ruim = pred_comparacao(TOKEN_INT,   &id,    &n10);   /* operador invalido */
    Predicado curto_and = pred_and(&c_lt, &c_idade);
    Predicado curto_or  = pred_or(&c_ge, &c_idade);
    Predicado forca_and = pred_and(&c_ge, &c_idade);
    Predicado sem_dir   = pred_and(&c_ge, NULL);

    printf("\n-- curto-circuito e erros\n");
    demo_pred("id<10 AND idade=10 (curto-circuito)",  &curto_and, &row);
    demo_pred("id>=30 OR idade=10 (curto-circuito)",  &curto_or,  &row);
    demo_pred("id>=30 AND idade=10 (coluna inexist.)", &forca_and, &row);
    demo_pred("nome = 10 (tipos incompativeis)",      &c_tipo,    &row);
    demo_pred("id <operador invalido> 10",            &c_op_ruim, &row);
    demo_pred("id>=30 AND <ausente>",                 &sem_dir,   &row);

    /* 4) Via Parser: WHERE com varias condicoes */
    printf("\n-- WHERE via Parser\n");
    demo_pred_where(&row, "SELECT * FROM alunos WHERE id = 30;");
    demo_pred_where(&row, "SELECT * FROM alunos WHERE id >= 10 AND nome = 'Caio';");
    demo_pred_where(&row, "SELECT * FROM alunos WHERE id > 30;");
    demo_pred_where(&row, "SELECT * FROM alunos;");
    demo_pred_where(&row, "SELECT * FROM alunos WHERE id = 'abc';");
    return 0;
}
#endif /*PREDICATE_EVALUATOR_SEM_MAIN*/
#endif /* PREDICATE_EVALUATOR */
