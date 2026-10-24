/*
 * Analisador Lexico).
 *
 * Comandos reconhecidos:
 *   CREATE TABLE nome (coluna INT, coluna CHAR, ...);
 *   INSERT INTO nome VALUES (10, 'abc');
 *   SELECT * FROM nome WHERE coluna = 10 AND outra <> 'x';
 *
 * Tipos de dados: INT e CHAR (registros de tamanho fixo de 8 bytes).
 * NAO ha: juncao, ORDER BY, agregacao, subconsulta, OR.
 * Palavras dessas construcoes devolvem TOKEN_NAO_SUPORTADO.
 *
 * 
 
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ------------------------------------------------------------------ */
/* Constantes                                                          */
/* ------------------------------------------------------------------ */

#define TAM_REGISTRO 8   /* tamanho fixo do registro, em bytes          */
#define MAX_LEXEMA   64  /* tamanho maximo de um lexema (com o '\0')    */

/* ------------------------------------------------------------------ */
/* Tipos de token                                                      */
/* ------------------------------------------------------------------ */

typedef enum {
    /* Palavras reservadas (comandos) */
    TOKEN_CREATE,
    TOKEN_TABLE,
    TOKEN_INSERT,
    TOKEN_INTO,
    TOKEN_VALUES,
    TOKEN_SELECT,
    TOKEN_FROM,
    TOKEN_WHERE,
    TOKEN_AND,

    /* Tipos de dados */
    TOKEN_INT,
    TOKEN_CHAR,

    /* Literais e identificadores */
    TOKEN_IDENTACAO,
    TOKEN_NUMERO,
    TOKEN_STRING,

    /* Separadores e simbolos */
    TOKEN_ESPACO,
    TOKEN_ABRE_PARENTESE,
    TOKEN_FECHA_PARENTESE,
    TOKEN_VIRGULA,
    TOKEN_PONTO_VIRGULA,
    TOKEN_ASTERISCO,

    /* Operadores de comparacao (usados no WHERE) */
    TOKEN_IGUAL,
    TOKEN_DIFERENTE,
    TOKEN_MENOR,
    TOKEN_MENOR_IGUAL,
    TOKEN_MAIOR,
    TOKEN_MAIOR_IGUAL,

    /* Controle */
    TOKEN_NAO_SUPORTADO,   /* JOIN, ORDER, BY, GROUP, COUNT, OR, ...      */
    TOKEN_FIM,             /* final de texto                              */
    TOKEN_ERRO             /* caractere/lexema invalido                   */
} TipoToken;

static const char *nome_token(TipoToken token)
{
    switch (token) {
        case TOKEN_CREATE:          return "CREATE";
        case TOKEN_TABLE:           return "TABLE";
        case TOKEN_INSERT:          return "INSERT";
        case TOKEN_INTO:            return "INTO";
        case TOKEN_VALUES:          return "VALUES";
        case TOKEN_SELECT:          return "SELECT";
        case TOKEN_FROM:            return "FROM";
        case TOKEN_WHERE:           return "WHERE";
        case TOKEN_AND:             return "AND";
        case TOKEN_INT:             return "INT";
        case TOKEN_CHAR:            return "CHAR";
        case TOKEN_IDENTACAO:       return "IDENT";
        case TOKEN_NUMERO:          return "NUMERO";
        case TOKEN_STRING:          return "STRING";
        case TOKEN_ESPACO:          return "ESPACO";
        case TOKEN_ABRE_PARENTESE:  return "ABRE_PAR";
        case TOKEN_FECHA_PARENTESE: return "FECHA_PAR";
        case TOKEN_VIRGULA:         return "VIRGULA";
        case TOKEN_PONTO_VIRGULA:   return "PONTO_VIRGULA";
        case TOKEN_ASTERISCO:       return "ASTERISCO";
        case TOKEN_IGUAL:           return "IGUAL";
        case TOKEN_DIFERENTE:       return "DIFERENTE";
        case TOKEN_MENOR:           return "MENOR";
        case TOKEN_MENOR_IGUAL:     return "MENOR_IGUAL";
        case TOKEN_MAIOR:           return "MAIOR";
        case TOKEN_MAIOR_IGUAL:     return "MAIOR_IGUAL";
        case TOKEN_NAO_SUPORTADO:   return "NAO_SUPORTADO";
        case TOKEN_FIM:             return "FIM";
        case TOKEN_ERRO:            return "ERRO";
    }
    return "?";
}

/* ------------------------------------------------------------------ */
/* Estruturas                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    TipoToken tipo;
    char      lexema[MAX_LEXEMA];
    int       linha;
    int       coluna;
} Token;

typedef struct {
    const char *src;     /* texto de entrada                            */
    size_t      posicao;     /* posicao atual                               */
    int         linha;
    int         coluna;
} Tokenizador;

/* ------------------------------------------------------------------ */
/* Tabela de palavras reservadas                                       */
/* A busca e sequencial e tem prioridade sobre TOKEN_IDENTACAO: se a palavra */
/* estiver aqui, nunca vira identificador.                             */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *palavra;
    TipoToken   tipo;
} Reservada;

static const Reservada RESERVADAS[] = {
    /* suportadas */
    { "CREATE", TOKEN_CREATE }, { "TABLE",  TOKEN_TABLE  },
    { "INSERT", TOKEN_INSERT }, { "INTO",   TOKEN_INTO   },
    { "VALUES", TOKEN_VALUES }, { "SELECT", TOKEN_SELECT },
    { "FROM",   TOKEN_FROM   }, { "WHERE",  TOKEN_WHERE  },
    { "AND",    TOKEN_AND    },
    { "INT",    TOKEN_INT    }, { "CHAR",   TOKEN_CHAR   },

    /* fora do escopo: OR, juncao, ORDER BY, agregacao, GROUP BY */
    { "OR",     TOKEN_NAO_SUPORTADO },
    { "JOIN",   TOKEN_NAO_SUPORTADO }, { "INNER",  TOKEN_NAO_SUPORTADO },
    { "LEFT",   TOKEN_NAO_SUPORTADO }, { "RIGHT",  TOKEN_NAO_SUPORTADO },
    { "ON",     TOKEN_NAO_SUPORTADO },
    { "ORDER",  TOKEN_NAO_SUPORTADO }, { "BY",     TOKEN_NAO_SUPORTADO },
    { "GROUP",  TOKEN_NAO_SUPORTADO }, { "HAVING", TOKEN_NAO_SUPORTADO },
    { "COUNT",  TOKEN_NAO_SUPORTADO }, { "SUM",    TOKEN_NAO_SUPORTADO },
    { "AVG",    TOKEN_NAO_SUPORTADO }, { "MIN",    TOKEN_NAO_SUPORTADO },
    { "MAX",    TOKEN_NAO_SUPORTADO },
};

#define NUM_RESERVADAS (sizeof(RESERVADAS) / sizeof(RESERVADAS[0]))

/* Comparacao sem diferenciar maiusculas/minusculas */
static int contexto_igual(const char *a, const char *b)
{
    while (*a && *b) {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static TipoToken classificar_palavra(const char *lexer)
{
    for (size_t i = 0; i < NUM_RESERVADAS; i++)
        if (contexto_igual(lexer, RESERVADAS[i].palavra))
            return RESERVADAS[i].tipo;
    return TOKEN_IDENTACAO;
}

/* ------------------------------------------------------------------ */
/* Funcoes auxiliares                                                  */
/* ------------------------------------------------------------------ */

void iniciar_tokenizador(Tokenizador *token, const char *texto)
{
    token->src    = texto;
    token->posicao    = 0;
    token->linha  = 1;
    token->coluna = 1;
}

static int olhar(const Tokenizador *t)
{
    return (unsigned char)t->src[t->posicao];
}

static int avancar(Tokenizador *t)
{
    int caractere = olhar(t);
    if (caractere == '\0')
        return '\0';
    t->posicao++;
    if (caractere == '\n') { t->linha++; t->coluna = 1; }
    else {t->coluna++; }
    return caractere;
}

static void adicionar_caractere(Token *token, size_t *n, int caractere)
{
    if (*n < MAX_LEXEMA - 1)
        token->lexema[(*n)++] = (char)caractere;
    token->lexema[*n] = '\0';
}

/* ------------------------------------------------------------------ */
/* Funcao principal: devolve o proximo token, na ordem do texto        */
/* ------------------------------------------------------------------ */

Token proximo_token(Tokenizador *t)
{
    Token token;
    size_t n = 0;
    int caractere;

    memset(&token, 0, sizeof token);
    token.linha  = t->linha;
    token.coluna = t->coluna;
    caractere = olhar(t);

    /* 1. Final de texto */
    if (caractere == '\0') {
        token.tipo = TOKEN_FIM;
        return token;
    }

    /* 2. Espaco (espaco, tab, \n, \r) - uma sequencia vira um token */
    if (isspace(caractere)) {
        while (isspace(olhar(t)))
            avancar(t);
        token.tipo = TOKEN_ESPACO;
        strcpy(token.lexema, " ");
        return token;
    }

    /* 3. Simbolos de um caractere */
    switch (caractere) {
        case '(': token.tipo = TOKEN_ABRE_PARENTESE;   break;
        case ')': token.tipo = TOKEN_FECHA_PARENTESE;  break;
        case ',': token.tipo = TOKEN_VIRGULA;          break;
        case ';': token.tipo = TOKEN_PONTO_VIRGULA;    break;
        case '*': token.tipo = TOKEN_ASTERISCO;        break;
        case '=': token.tipo = TOKEN_IGUAL;            break;
        default:  break;
    }
    if (caractere == '(' || caractere == ')' || caractere == ',' || caractere == ';' ||
        caractere == '*' || caractere == '=') {
        adicionar_caractere(&token, &n, avancar(t));
        return token;
    }

    /* 4. Operadores de 1 ou 2 caracteres: < <= <> > >= != */
    if (caractere == '<' || caractere == '>' || caractere == '!') {
        adicionar_caractere(&token, &n, avancar(t));
        int proximo = olhar(t);

        if (caractere == '<') {
            if (proximo == '=')      { adicionar_caractere(&token, &n, avancar(t)); token.tipo = TOKEN_MENOR_IGUAL; }
            else if (proximo == '>') { adicionar_caractere(&token, &n, avancar(t)); token.tipo = TOKEN_DIFERENTE;   }
            else                  { token.tipo = TOKEN_MENOR; }
        } else if (caractere == '>') {
            if (proximo == '=')      { adicionar_caractere(&token, &n, avancar(t)); token.tipo = TOKEN_MAIOR_IGUAL; }
            else                  { token.tipo = TOKEN_MAIOR; }
        } else { /* '!' so e valido como "!=" */
            if (proximo == '=')      { adicionar_caractere(&token, &n, avancar(t)); token.tipo = TOKEN_DIFERENTE; }
            else                  { token.tipo = TOKEN_ERRO; }
        }
        return token;
    }

    /* 5. Literal CHAR: 'texto' (no maximo TAM_REGISTRO caracteres) */
    if (caractere == '\'') {
        size_t tamanho = 0;
        avancar(t);                          /* abre aspas */
        while (olhar(t) != '\'' && olhar(t) != '\0') {
            adicionar_caractere(&token, &n, avancar(t));
            tamanho++;
        }
        if (olhar(t) != '\'') {              /* chegou ao fim sem fechar */
            token.tipo = TOKEN_ERRO;
            return token;
        }
        avancar(t);                          /* fecha aspas */
        token.tipo = (tamanho > TAM_REGISTRO) ? TOKEN_ERRO : TOKEN_STRING;
        return token;
    }

    /* 6. Literal INT: sequencia de digitos */
    if (isdigit(caractere)) {
        while (isdigit(olhar(t)))
            adicionar_caractere(&token, &n, avancar(t));
        /* numero colado em letra (ex.: 12abc) e invalido */
        if (isalpha(olhar(t)) || olhar(t) == '_') {
            while (isalnum(olhar(t)) || olhar(t) == '_')
                adicionar_caractere(&token, &n, avancar(t));
            token.tipo = TOKEN_ERRO;
        } else {
            token.tipo = TOKEN_NUMERO;
        }
        return token;
    }

    /* 7. Identificador ou palavra reservada */
    if (isalpha(caractere) || caractere == '_') {
        int estourou = 0;
        while (isalnum(olhar(t)) || olhar(t) == '_') {
            if (n >= MAX_LEXEMA - 1) estourou = 1;
            adicionar_caractere(&token, &n, avancar(t));
        }
        token.tipo = estourou ? TOKEN_ERRO : classificar_palavra(token.lexema);
        return token;
    }

    /* 8. Qualquer outro caractere e invalido */
    adicionar_caractere(&token, &n, avancar(t));
    token.tipo = TOKEN_ERRO;
    return token;
}

/* ------------------------------------------------------------------ */
/* Programa de teste                                                   */
/* ------------------------------------------------------------------ */

static char *ler_arquivo(const char *caminho)
{
    FILE *f = fopen(caminho, "rb");
    if (!f) return NULL;

    size_t capacidade = 1024, tamanho = 0;
    char *buffer = malloc(capacidade);
    if (!buffer) { fclose(f); return NULL; }

    size_t lidos;
    while ((lidos = fread(buffer + tamanho, 1, capacidade - tamanho - 1, f)) > 0) {
        tamanho += lidos;
        if (tamanho + 1 >= capacidade) {
            capacidade *= 2;
            char *novo = realloc(buffer, capacidade);
            if (!novo) { free(buffer); fclose(f); return NULL; }
            buffer = novo;
        }
    }
    buffer[tamanho] = '\0';
    fclose(f);
    return buffer;
}

int main(int argc, char **argv)
{
    static const char *EXEMPLO =
        "CREATE TABLE alunos (id INT, nome CHAR);\n"
        "INSERT INTO alunos VALUES (1, 'Ana');\n"
        "SELECT * FROM alunos WHERE id >= 1 AND nome <> 'Bia';";

    char *arquivo = NULL;
    const char *texto = EXEMPLO;

    if (argc > 1) {
        arquivo = ler_arquivo(argv[1]);
        if (!arquivo) {
            fprintf(stderr, "Erro: nao foi possivel ler '%s'\n", argv[1]);
            return 1;
        }
        texto = arquivo;
    }

    Tokenizador t;
    iniciar_tokenizador(&t, texto);

    printf("%-4s %-14s %-8s %s\n", "#", "TOKEN", "POSICAO", "LEXEMA");
    printf("--------------------------------------------------\n");

    int ordem = 0;
    int status = 0;
    Token token;
    do {
        token = proximo_token(&t);
        printf("%-4d %-14s %d:%-5d '%s'\n",
               ++ordem, nome_token(token.tipo), token.linha, token.coluna, token.lexema);

        if (token.tipo == TOKEN_ERRO || token.tipo == TOKEN_NAO_SUPORTADO)
            status = 1;
    } while (token.tipo != TOKEN_FIM);

    free(arquivo);
    return status;
}
