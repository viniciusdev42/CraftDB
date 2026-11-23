/* ============================================================
 *                 EXPRESSION EVALUATOR
 * ============================================================
 *
 * Avalia expressoes da AST e produz valores TIPADOS (Value).
 *
 * Nos suportados:
 *   - EXPR_LITERAL    : devolve o valor embutido no no.
 *   - EXPR_COLUNA     : busca o valor da coluna na Row (por nome).
 *   - EXPR_COMPARACAO : avalia os dois filhos e compara (=, <>, <, <=, >, >=),
 *                       produzindo um Value do tipo VALOR_BOOL.
 *
 * Isolamento do armazenamento fisico:
 *   O avaliador so conhece a Row (representacao LOGICA da linha) e usa apenas a
 *   sua API publica (row_posicao_da_coluna, row_tipo_da_coluna, row_obter_int,
 *   row_obter_char). Nao ha acesso a paginas, cache, arvore B+ ou registros de
 *   8 bytes. Trocar o formato de armazenamento nao afeta este modulo.
 *
 * Regras de tipo:
 *   - INT   compara com INT   (comparacao numerica).
 *   - CHAR  compara com CHAR  (strcmp, sensivel a maiusculas/minusculas).
 *   - INT contra CHAR e' erro de tipo; nao ha conversao implicita.
 *   - Nomes de coluna nao diferenciam maiusculas/minusculas (regra da Row).
 *
 * Convencao de retorno (igual a da Row): 0 = sucesso, -1 = erro.
 * A mensagem do erro fica em Avaliador.mensagem.
 *
 * Ciclo de vida dos nos: o chamador e' dono dos nos (sem malloc); os filhos de
 * uma comparacao sao ponteiros e devem viver ate o fim da avaliacao.
 */

#ifndef EXPRESSION_EVALUATOR
#define EXPRESSION_EVALUATOR

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "Parser.c"

#define main ROW_SEM_MAIN
#include "Row.c"
#undef main
/* ------------------------------------------------------------------ */
/* Value: resultado tipado de uma expressao                            */
/* ------------------------------------------------------------------ */

typedef enum {
    VALOR_INT,
    VALOR_CHAR,
    VALOR_BOOL
} TipoValor;

/* Comporta tanto um lexema de literal quanto o texto de uma coluna CHAR. */
#define MAX_TEXTO_VALOR (MAX_LEXEMA > (TAM_CHAR + 1) ? MAX_LEXEMA : (TAM_CHAR + 1))

/* Somente o membro correspondente a 'tipo' e' significativo. */
typedef struct {
    TipoValor tipo;
    int32_t   inteiro;                  /* VALOR_INT  */
    char      texto[MAX_TEXTO_VALOR];   /* VALOR_CHAR */
    int       booleano;                 /* VALOR_BOOL: 0 ou 1 */
} Value;

static Value valor_int(int32_t v)
{
    Value valor;
    memset(&valor, 0, sizeof valor);
    valor.tipo    = VALOR_INT;
    valor.inteiro = v;
    return valor;
}

static Value valor_char(const char *texto)
{
    Value valor;
    memset(&valor, 0, sizeof valor);
    valor.tipo = VALOR_CHAR;
    snprintf(valor.texto, sizeof valor.texto, "%s", texto != NULL ? texto : "");
    return valor;
}

static Value valor_bool(int b)
{
    Value valor;
    memset(&valor, 0, sizeof valor);
    valor.tipo     = VALOR_BOOL;
    valor.booleano = b ? 1 : 0;
    return valor;
}

static const char *nome_tipo_valor(TipoValor tipo)
{
    switch (tipo) {
        case VALOR_INT:  return "INT";
        case VALOR_CHAR: return "CHAR";
        case VALOR_BOOL: return "BOOL";
    }
    return "?";
}

/* ------------------------------------------------------------------ */
/* Nos da expressao                                                    */
/* ------------------------------------------------------------------ */

typedef enum {
    EXPR_LITERAL,
    EXPR_COLUNA,
    EXPR_COMPARACAO
} TipoExpressao;

typedef struct Expressao Expressao;
struct Expressao {
    TipoExpressao tipo;

    Value         literal;              /* EXPR_LITERAL                       */
    char          coluna[MAX_LEXEMA];   /* EXPR_COLUNA                        */

    TipoToken        operador;          /* EXPR_COMPARACAO: TOKEN_IGUAL, ...  */
    const Expressao *esquerda;
    const Expressao *direita;
};

static Expressao expr_literal(Value valor)
{
    Expressao e;
    memset(&e, 0, sizeof e);
    e.tipo    = EXPR_LITERAL;
    e.literal = valor;
    return e;
}

static Expressao expr_coluna(const char *nome)
{
    Expressao e;
    memset(&e, 0, sizeof e);
    e.tipo = EXPR_COLUNA;
    snprintf(e.coluna, sizeof e.coluna, "%s", nome != NULL ? nome : "");
    return e;
}

static Expressao expr_comparacao(TipoToken operador, const Expressao *esq, const Expressao *dir)
{
    Expressao e;
    memset(&e, 0, sizeof e);
    e.tipo     = EXPR_COMPARACAO;
    e.operador = operador;
    e.esquerda = esq;
    e.direita  = dir;
    return e;
}

/* ------------------------------------------------------------------ */
/* Adaptadores da AST do Parser (Valor / Condicao) para Expressao      */
/* ------------------------------------------------------------------ */

/* Valor (NUMERO ou STRING) -> no literal. */
static Expressao expr_de_valor(const Valor *v)
{
    if (v->tipo == TOKEN_NUMERO)
        return expr_literal(valor_int((int32_t)v->numero));
    return expr_literal(valor_char(v->str));
}

/* Uma Condicao "coluna op valor" vira uma pequena arvore de 3 nos. O conjunto
   e' agrupado numa struct para que o chamador mantenha os nos vivos sem malloc. */
typedef struct {
    Expressao coluna;
    Expressao literal;
    Expressao comparacao;   /* raiz da arvore */
} ExprCondicao;

static void expr_de_condicao(ExprCondicao *dest, const Condicao *c)
{
    dest->coluna     = expr_coluna(c->coluna);
    dest->literal    = expr_de_valor(&c->valor);
    dest->comparacao = expr_comparacao(c->operacao, &dest->coluna, &dest->literal);
}

/* ------------------------------------------------------------------ */
/* Avaliador                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    char mensagem[MAX_MESSAGEM];    /* ultima mensagem de erro */
} Avaliador;

static void iniciar_avaliador(Avaliador *av)
{
    av->mensagem[0] = '\0';
}

/* Grava a mensagem e devolve sempre -1, permitindo "return av_erro(...)". */
static int av_erro(Avaliador *av, const char *formato, ...)
{
    va_list ap;
    va_start(ap, formato);
    vsnprintf(av->mensagem, sizeof av->mensagem, formato, ap);
    va_end(ap);
    return -1;
}

static const char *simbolo_comparacao(TipoToken op)
{
    return simbolo_operacao(op);    /* definido no Parser */
}

/* Coluna -> Value, lendo apenas pela API logica da Row. */
static int avaliar_coluna(Avaliador *av, const Expressao *expr, const Row *row, Value *saida)
{
    TipoToken tipo;
    int pos = row_posicao_da_coluna(row, expr->coluna);

    if (pos < 0)
        return av_erro(av, "coluna '%s' nao existe na tabela '%s'",
                       expr->coluna, row->tabela);
    if (row_tipo_da_coluna(row, pos, &tipo) != 0)
        return av_erro(av, "nao foi possivel obter o tipo da coluna '%s'", expr->coluna);

    if (tipo == TOKEN_INT) {
        int32_t v;
        if (row_obter_int(row, pos, &v) != 0)
            return av_erro(av, "falha ao ler a coluna INT '%s'", expr->coluna);
        *saida = valor_int(v);
        return 0;
    }

    if (tipo == TOKEN_CHAR) {
        const char *texto = row_obter_char(row, pos);
        if (texto == NULL)
            return av_erro(av, "falha ao ler a coluna CHAR '%s'", expr->coluna);
        *saida = valor_char(texto);
        return 0;
    }

    return av_erro(av, "coluna '%s' possui tipo nao suportado", expr->coluna);
}

/* Compara dois Values ja avaliados e devolve VALOR_BOOL. */
static int comparar_valores(Avaliador *av, TipoToken op, const Value *a, const Value *b,
                            Value *saida)
{
    int cmp;

    if (a->tipo != b->tipo)
        return av_erro(av, "tipos incompativeis na comparacao: %s %s %s",
                       nome_tipo_valor(a->tipo), simbolo_comparacao(op),
                       nome_tipo_valor(b->tipo));

    switch (a->tipo) {
        case VALOR_INT:
            cmp = (a->inteiro > b->inteiro) - (a->inteiro < b->inteiro);
            break;
        case VALOR_CHAR:
            cmp = strcmp(a->texto, b->texto);
            break;
        default:
            return av_erro(av, "comparacao nao suportada para o tipo %s",
                           nome_tipo_valor(a->tipo));
    }

    switch (op) {
        case TOKEN_IGUAL:       *saida = valor_bool(cmp == 0); return 0;
        case TOKEN_DIFERENTE:   *saida = valor_bool(cmp != 0); return 0;
        case TOKEN_MENOR:       *saida = valor_bool(cmp <  0); return 0;
        case TOKEN_MENOR_IGUAL: *saida = valor_bool(cmp <= 0); return 0;
        case TOKEN_MAIOR:       *saida = valor_bool(cmp >  0); return 0;
        case TOKEN_MAIOR_IGUAL: *saida = valor_bool(cmp >= 0); return 0;
        default:
            return av_erro(av, "operador de comparacao invalido");
    }
}

/*
 avaliar_expressao(av, expr, row, saida)
 Identifica o tipo do no e o avalia:
   literal    -> o proprio valor
   coluna     -> o valor da coluna na Row
   comparacao -> BOOL com o resultado de esquerda <op> direita
 'row' pode ser NULL apenas se a expressao nao referenciar colunas.
 Retorna 0 em sucesso, -1 em erro (mensagem em av->mensagem).
*/
static int avaliar_expressao(Avaliador *av, const Expressao *expr, const Row *row, Value *saida)
{
    if (av == NULL)
        return -1;
    if (expr == NULL || saida == NULL)
        return av_erro(av, "avaliar_expressao recebeu argumento NULL");

    switch (expr->tipo) {
        case EXPR_LITERAL:
            *saida = expr->literal;
            return 0;

        case EXPR_COLUNA:
            if (row == NULL)
                return av_erro(av, "coluna '%s' referenciada sem uma Row", expr->coluna);
            return avaliar_coluna(av, expr, row, saida);

        case EXPR_COMPARACAO: {
            Value esq, dir;
            if (expr->esquerda == NULL || expr->direita == NULL)
                return av_erro(av, "comparacao com operando ausente");
            if (avaliar_expressao(av, expr->esquerda, row, &esq) != 0) return -1;
            if (avaliar_expressao(av, expr->direita,  row, &dir) != 0) return -1;
            return comparar_valores(av, expr->operador, &esq, &dir, saida);
        }
    }
    return av_erro(av, "tipo de no desconhecido");
}

/*
 avaliar_where(av, comando, row, resultado)
 Avalia todas as condicoes do WHERE de um SELECT (ligadas por AND, com curto-
 circuito). Sem condicoes, o resultado e' 1. *resultado recebe 0 ou 1.
 Retorna 0 em sucesso, -1 em erro.
*/
static int avaliar_where(Avaliador *av, const Comando *comando, const Row *row, int *resultado)
{
    if (av == NULL)
        return -1;
    if (comando == NULL || resultado == NULL)
        return av_erro(av, "avaliar_where recebeu argumento NULL");

    *resultado = 1;
    for (int i = 0; i < comando->numero_condicoes; i++) {
        ExprCondicao arvore;
        Value        v;

        expr_de_condicao(&arvore, &comando->condicoes[i]);
        if (avaliar_expressao(av, &arvore.comparacao, row, &v) != 0)
            return -1;
        if (!v.booleano) {
            *resultado = 0;
            return 0;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Demonstracao (compile com -DEXPRESSION_EVALUATOR_SEM_MAIN)              */
/* ------------------------------------------------------------------ */

/* Monta a Row na mao: o avaliador nao precisa de catalogo nem de disco. */
static void demo_campo(Row *row, const char *nome, TipoToken tipo, int32_t inteiro, const char *texto)
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

static void demo_where(const Row *row, const char *sql)
{
    Parser    parser;
    Comando   cmd;
    Avaliador av;
    int       ok;

    iniciar_parser(&parser, sql);
    iniciar_avaliador(&av);
    if (!parse_comando(&parser, &cmd)) {
        printf("%-58s -> erro de sintaxe: %s\n", sql, parser.mensagem);
        return;
    }
    if (avaliar_where(&av, &cmd, row, &ok) != 0)
        printf("%-58s -> erro: %s\n", sql, av.mensagem);
    else
        printf("%-58s -> %s\n", sql, ok ? "verdadeiro" : "falso");
}
#ifdef EXPRESSION_EVALUATOR_SEM_MAIN
int main(void)
{
    Row       row;
    Avaliador av;
    Value     v;

    memset(&row, 0, sizeof row);
    strcpy(row.tabela, "alunos");
    demo_campo(&row, "id",   TOKEN_INT,  30, NULL);
    demo_campo(&row, "nome", TOKEN_CHAR, 0,  "Caio");

    iniciar_avaliador(&av);

    /* 1) Arvore montada manualmente: id >= 10 */
    Expressao col = expr_coluna("ID");                  /* sem case */
    Expressao lit = expr_literal(valor_int(10));
    Expressao cmp = expr_comparacao(TOKEN_MAIOR_IGUAL, &col, &lit);

    if (avaliar_expressao(&av, &col, &row, &v) == 0)
        printf("coluna id          -> %s %d\n", nome_tipo_valor(v.tipo), v.inteiro);
    if (avaliar_expressao(&av, &lit, &row, &v) == 0)
        printf("literal 10         -> %s %d\n", nome_tipo_valor(v.tipo), v.inteiro);
    if (avaliar_expressao(&av, &cmp, &row, &v) == 0)
        printf("id >= 10           -> %s %d\n", nome_tipo_valor(v.tipo), v.booleano);

    /* 2) Erros de tipo e de coluna */
    Expressao col_nome = expr_coluna("nome");
    Expressao cmp_erro = expr_comparacao(TOKEN_IGUAL, &col_nome, &lit);
    if (avaliar_expressao(&av, &cmp_erro, &row, &v) != 0)
        printf("nome = 10          -> erro: %s\n", av.mensagem);

    Expressao col_x = expr_coluna("idade");
    if (avaliar_expressao(&av, &col_x, &row, &v) != 0)
        printf("coluna idade       -> erro: %s\n", av.mensagem);

    /* 3) Via Parser: WHERE com varias condicoes */
    printf("\n");
    demo_where(&row, "SELECT * FROM alunos WHERE id = 30;");
    demo_where(&row, "SELECT * FROM alunos WHERE id > 30;");
    demo_where(&row, "SELECT * FROM alunos WHERE id >= 10 AND nome = 'Caio';");
    demo_where(&row, "SELECT * FROM alunos WHERE nome <> 'Caio';");
    demo_where(&row, "SELECT * FROM alunos WHERE nome < 'Dora';");
    demo_where(&row, "SELECT * FROM alunos;");
    demo_where(&row, "SELECT * FROM alunos WHERE id = 'abc';");
    return 0;

}
#endif /*EXPRESSION_EVALUATOR_SEM_MAIN*/
#endif /* EXPRESSION_EVALUATOR */
