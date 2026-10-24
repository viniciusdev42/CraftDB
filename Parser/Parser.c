/*
 * Analisador Sintatico - descendente recursivo.
 *
 * Usa o Analisador Lexico do Tokenizer.

 * Gramatica (palavras reservadas sem diferenciar maiusculas/minusculas):
 *
 *   programa   ::= { comando }  FINAL
 *   comando    ::= create | insert | select
 *
 *   create     ::= CREATE TABLE IDENT '(' coluna { ',' coluna } ')' ';'
 *   coluna     ::= IDENT ( INT | CHAR )
 *
 *   insert     ::= INSERT INTO IDENT VALUES '(' valor { ',' valor } ')' ';'
 *
 *   select     ::= SELECT '*' FROM IDENT [ WHERE condicao { AND condicao } ] ';'
 *   condicao   ::= IDENT operador valor
 *   operador   ::= '=' | '<>' | '!=' | '<' | '<=' | '>' | '>='
 *
 *   valor      ::= NUMERO | STRING
 *
 * Tokens TOKEN_ERRO e TOKEN_NAO_SUPORTADO (JOIN, OR, ORDER BY, ...) geram
 * mensagens de erro especificas. Apos um erro, o parser descarta tokens
 * ate o proximo ';' e continua a analisar os comandos seguintes.
 */

#include <errno.h>
#include <limits.h>
#include <stdarg.h>

/*
 * Reaproveita o tokenizador sem alterar o arquivo original: o main() do
 * Tokenizer.c e renomeado so durante a inclusao, para nao colidir com o
 * main() deste arquivo.
 */
#define main PARSER_TOKENIZER_MAIN
#include "Tokenizer.c"
#undef main

/* ------------------------------------------------------------------ */
/* Limites                                                             */
/* ------------------------------------------------------------------ */

#define MAX_COLUNAS   16   /* colunas por tabela                        */
#define MAX_VALORES   16   /* valores por INSERT                        */
#define MAX_CONDICOES 16   /* condicoes por WHERE                       */
#define MAX_MESSAGEM  256  /* tamanho das mensagens de erro             */

/* ------------------------------------------------------------------ */
/* Arvore sintatica (AST)                                              */
/* ------------------------------------------------------------------ */

typedef enum {
    CMD_CREATE,
    CMD_INSERT,
    CMD_SELECT
} TipoComando;

typedef struct {
    char      nome[MAX_LEXEMA];
    TipoToken tipo;                 /* TOKEN_INT ou TOKEN_CHAR              */
} Coluna;

typedef struct {
    TipoToken tipo;                 /* TOKEN_NUMERO ou TOKEN_STRING            */
    int       numero;                  /* valido se tipo == TOKEN_NUMERO        */
    char      str[MAX_LEXEMA];      /* valido se tipo == TOKEN_STRING     */
} Valor;

typedef struct {
    char      coluna[MAX_LEXEMA];
    TipoToken operacao;                   /* TOKEN_IGUAL, TOKEN_MENOR, ...        */
    Valor     valor;
} Condicao;

typedef struct {
    TipoComando tipo;
    char        tabela[MAX_LEXEMA];

    Coluna      colunas[MAX_COLUNAS];       /* CREATE TABLE             */
    int         numero_colunas;

    Valor       valores[MAX_VALORES];       /* INSERT                   */
    int         numero_valores;

    Condicao    condicoes[MAX_CONDICOES];   /* SELECT ... WHERE         */
    int         numero_condicoes;
} Comando;

/* ------------------------------------------------------------------ */
/* Estado do parser                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    Tokenizador lexer;
    Token       atual;              /* token corrente (lookahead)       */
    char        mensagem[MAX_MESSAGEM];       /* ultima mensagem de erro          */
} Parser;

/* Avanca para o proximo token ignorando espacos em branco */
static void avancar_parser(Parser *parser)
{
    do {
        parser->atual = proximo_token(&parser->lexer);
    } while (parser->atual.tipo == TOKEN_ESPACO);
}

static void iniciar_parser(Parser *parser, const char *texto)
{
    iniciar_tokenizador(&parser->lexer, texto);
    parser->mensagem[0] = '\0';
    avancar_parser(parser);
}

/* ------------------------------------------------------------------ */
/* Tratamento de erros                                                 */
/* ------------------------------------------------------------------ */

/* Grava em parser->mensagem: "Linha L, coluna C: <mensagem>". Devolve sempre 0. */
static int erro_formato(Parser *parser, const Token *token, const char *formato, ...)
{
    int numero_linha_coluna = snprintf(parser->mensagem, MAX_MESSAGEM, "Linha %d, coluna %d: ",
                     token->linha, token->coluna);
    if (numero_linha_coluna < 0 || numero_linha_coluna >= MAX_MESSAGEM)
        return 0;

    va_list ap;
    va_start(ap, formato);
    vsnprintf(parser->mensagem + numero_linha_coluna, MAX_MESSAGEM - (size_t)numero_linha_coluna, formato, ap);
    va_end(ap);
    return 0;
}

/* Erro "token inesperado": descreve o que foi encontrado e o que se esperava */
static int falha(Parser *parser, const char *esperado)
{
    const Token *token = &parser->atual;

    switch (token->tipo) {
        case TOKEN_ERRO:
            return erro_formato(parser, token, "token invalido '%s'", token->lexema);
        case TOKEN_NAO_SUPORTADO:
            return erro_formato(parser, token, "construcao nao suportada '%s'", token->lexema);
        case TOKEN_FIM:
            return erro_formato(parser, token, "fim inesperado do texto (esperado: %s)",
                            esperado);
        default:
            return erro_formato(parser, token, "encontrado '%s' (esperado: %s)",
                            token->lexema, esperado);
    }
}

/* Consome o token se for do tipo esperado; caso contrario, gera erro */
static int consumir_token(Parser *parser, TipoToken tipo, const char *descricao,
                    Token *saida)
{
    if (parser->atual.tipo != tipo)
        return falha(parser, descricao);
    if (saida)
        *saida = parser->atual;
    avancar_parser(parser);
    return 1;
}

/* Recuperacao de erro: descarta tokens ate depois do proximo ';' */
static void sincronizar(Parser *parser)
{
    while (parser->atual.tipo != TOKEN_FIM && parser->atual.tipo != TOKEN_PONTO_VIRGULA)
        avancar_parser(parser);
    if (parser->atual.tipo == TOKEN_PONTO_VIRGULA)
        avancar_parser(parser);
}

/* ------------------------------------------------------------------ */
/* Regras gramaticais                                                */
/* ------------------------------------------------------------------ */

static int e_operador(TipoToken t)
{
    switch (t) {
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

/* valor ::= NUMERO | STRING */
static int parse_valor(Parser *parser, Valor *valor)
{
    Token token = parser->atual;

    if (token.tipo == TOKEN_NUMERO) {
        char *fim;
        errno = 0;
        long x = strtol(token.lexema, &fim, 10);
        if (errno == ERANGE || x > INT_MAX)
            return erro_formato(parser, &token, "numero '%s' fora do intervalo de INT",
                            token.lexema);
        valor->tipo   = TOKEN_NUMERO;
        valor->numero    = (int)x;
        valor->str[0] = '\0';
    } else if (token.tipo == TOKEN_STRING) {
        valor->tipo = TOKEN_STRING;
        valor->numero  = 0;
        strcpy(valor->str, token.lexema);
    } else {
        return falha(parser, "numero ou string");
    }

    avancar_parser(parser);
    return 1;
}

/* CREATE TABLE IDENT '(' coluna { ',' coluna } ')' ';'
 * (o token CREATE ja foi verificado por parse_comando) */
static int parse_create(Parser *parser, Comando *comando)
{
    Token token;

    comando->tipo = CMD_CREATE;
    avancar_parser(parser);                                   /* CREATE */

    if (!consumir_token(parser, TOKEN_TABLE, "TABLE", NULL))  return 0;
    if (!consumir_token(parser, TOKEN_IDENTACAO, "nome da tabela", &token))  return 0;
    strcpy(comando->tabela, token.lexema);
    if (!consumir_token(parser, TOKEN_ABRE_PARENTESE, "'('", NULL)) return 0;

    for (;;) {
        Token  nome;
        Coluna *coluna;

        if (!consumir_token(parser, TOKEN_IDENTACAO, "nome da coluna", &nome))
            return 0;

        if (comando->numero_colunas >= MAX_COLUNAS)
            return erro_formato(parser, &nome, "numero maximo de colunas (%d) excedido",
                            MAX_COLUNAS);

        for (int i = 0; i < comando->numero_colunas; i++)
            if (contexto_igual(comando->colunas[i].nome, nome.lexema))
                return erro_formato(parser, &nome, "coluna '%s' duplicada",
                                nome.lexema);

        coluna = &comando->colunas[comando->numero_colunas];
        strcpy(coluna->nome, nome.lexema);

        if (parser->atual.tipo != TOKEN_INT && parser->atual.tipo != TOKEN_CHAR)
            return falha(parser, "INT ou CHAR");
        coluna->tipo = parser->atual.tipo;
        avancar_parser(parser);
        comando->numero_colunas++;

        if (parser->atual.tipo == TOKEN_VIRGULA) {
            avancar_parser(parser);
            continue;
        }
        break;
    }

    if (!consumir_token(parser, TOKEN_FECHA_PARENTESE, "')' ou ','", NULL)) return 0;
    if (!consumir_token(parser, TOKEN_PONTO_VIRGULA, "';'", NULL)) return 0;
    return 1;
}

/* INSERT INTO IDENT VALUES '(' valor { ',' valor } ')' ';' */
static int parse_insert(Parser *parser, Comando *comando)
{
    Token token;

    comando->tipo = CMD_INSERT;
    avancar_parser(parser);                                   /* INSERT */

    if (!consumir_token(parser, TOKEN_INTO, "INTO", NULL))            return 0;
    if (!consumir_token(parser, TOKEN_IDENTACAO, "nome da tabela", &token))  return 0;
    strcpy(comando->tabela, token.lexema);
    if (!consumir_token(parser, TOKEN_VALUES, "VALUES", NULL))        return 0;
    if (!consumir_token(parser, TOKEN_ABRE_PARENTESE, "'('", NULL))         return 0;

    for (;;) {
        if (comando->numero_valores >= MAX_VALORES)
            return erro_formato(parser, &parser->atual,
                            "numero maximo de valores (%d) excedido",
                            MAX_VALORES);

        if (!parse_valor(parser, &comando->valores[comando->numero_valores]))
            return 0;
        comando->numero_valores++;

        if (parser->atual.tipo == TOKEN_VIRGULA) {
            avancar_parser(parser);
            continue;
        }
        break;
    }

    if (!consumir_token(parser, TOKEN_FECHA_PARENTESE, "')' ou ','", NULL))     return 0;
    if (!consumir_token(parser, TOKEN_PONTO_VIRGULA, "';'", NULL))        return 0;
    return 1;
}

/* condicao ::= IDENT operador valor */
static int parse_condicao(Parser *parser, Condicao *condicao)
{
    Token token;

    if (!consumir_token(parser, TOKEN_IDENTACAO, "nome da coluna", &token))
        return 0;
    strcpy(condicao->coluna, token.lexema);

    if (!e_operador(parser->atual.tipo))
        return falha(parser, "operador (=, <>, !=, <, <=, >, >=)");
    condicao->operacao = parser->atual.tipo;
    avancar_parser(parser);

    return parse_valor(parser, &condicao->valor);
}

/* SELECT '*' FROM IDENT [ WHERE condicao { AND condicao } ] ';' */
static int parse_select(Parser *parser, Comando *comando)
{
    Token token;

    comando->tipo = CMD_SELECT;
    avancar_parser(parser);                                   /* SELECT */

    if (!consumir_token(parser, TOKEN_ASTERISCO, "'*'", NULL))        return 0;
    if (!consumir_token(parser, TOKEN_FROM, "FROM", NULL))            return 0;
    if (!consumir_token(parser, TOKEN_IDENTACAO, "nome da tabela", &token))  return 0;
    strcpy(comando->tabela, token.lexema);

    if (parser->atual.tipo == TOKEN_WHERE) {
        avancar_parser(parser);

        for (;;) {
            if (comando->numero_condicoes >= MAX_CONDICOES)
                return erro_formato(parser, &parser->atual,
                                "numero maximo de condicoes (%d) excedido",
                                MAX_CONDICOES);

            if (!parse_condicao(parser, &comando->condicoes[comando->numero_condicoes]))
                return 0;
            comando->numero_condicoes++;

            if (parser->atual.tipo == TOKEN_AND) {
                avancar_parser(parser);
                continue;
            }
            break;
        }
    }

    if (!consumir_token(parser, TOKEN_PONTO_VIRGULA, "';', WHERE ou AND", NULL))
        return 0;
    return 1;
}

/* comando ::= create | insert | select */
static int parse_comando(Parser *parser, Comando *comando)
{
    memset(comando, 0, sizeof *comando);

    switch (parser->atual.tipo) {
        case TOKEN_CREATE: return parse_create(parser, comando);
        case TOKEN_INSERT: return parse_insert(parser, comando);
        case TOKEN_SELECT: return parse_select(parser, comando);
        default:         return falha(parser, "CREATE, INSERT ou SELECT");
    }
}

/* ------------------------------------------------------------------ */
/* Impressao da AST                                                    */
/* ------------------------------------------------------------------ */

static const char *simbolo_operacao(TipoToken operacao)
{
    switch (operacao) {
        case TOKEN_IGUAL:       return "=";
        case TOKEN_DIFERENTE:   return "<>";
        case TOKEN_MENOR:       return "<";
        case TOKEN_MENOR_IGUAL: return "<=";
        case TOKEN_MAIOR:       return ">";
        case TOKEN_MAIOR_IGUAL: return ">=";
        default:              return "?";
    }
}

static void imprimir_valor(const Valor *valor)
{
    if (valor->tipo == TOKEN_NUMERO)
        printf("%d", valor->numero);
    else
        printf("'%s'", valor->str);
}

static void imprimir_comando(const Comando *comando)
{
    switch (comando->tipo) {
        case CMD_CREATE:
            printf("CREATE TABLE\n");
            printf("  tabela : %s\n", comando->tabela);
            printf("  colunas:\n");
            for (int i = 0; i < comando->numero_colunas; i++)
                printf("    - %s : %s\n", comando->colunas[i].nome,
                       nome_token(comando->colunas[i].tipo));
            break;

        case CMD_INSERT:
            printf("INSERT\n");
            printf("  tabela : %s\n", comando->tabela);
            printf("  valores: ");
            for (int i = 0; i < comando->numero_valores; i++) {
                if (i) printf(", ");
                imprimir_valor(&comando->valores[i]);
            }
            printf("\n");
            break;

        case CMD_SELECT:
            printf("SELECT\n");
            printf("  tabela : %s\n", comando->tabela);
            if (comando->numero_condicoes == 0) {
                printf("  where  : (nenhum)\n");
            } else {
                printf("  where  :\n");
                for (int i = 0; i < comando->numero_condicoes; i++) {
                    printf("    %s %s %s", i ? "AND" : "   ",
                           comando->condicoes[i].coluna,
                           simbolo_operacao(comando->condicoes[i].operacao));
                    printf(" ");
                    imprimir_valor(&comando->condicoes[i].valor);
                    printf("\n");
                }
            }
            break;
    }
}
#endif
