#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define main MAIN_TOKENIZER
#include "Tokenizer.c"
#undef main

static int total_testes = 0;
static int testes_falhos = 0;

#define CHECK(cond, mensagem) do { \
    total_testes++; \
    if (!(cond)) { \
        testes_falhos++; \
        fprintf(stderr, "FALHOU: %s (linha %d)\n", mensagem, __LINE__); \
    } \
} while (0)

static void verificar_token(Tokenizador *t, TipoToken esperado,
                            const char *lexema, int linha, int coluna,
                            const char *descricao)
{
    Token token = proximo_token(t);
    CHECK(token.tipo == esperado, descricao);
    CHECK(strcmp(token.lexema, lexema) == 0, "lexema inesperado");
    CHECK(token.linha == linha, "linha inesperada");
    CHECK(token.coluna == coluna, "coluna inesperada");
}

static void testar_inicializacao_e_fim(void)
{
    Tokenizador t;
    iniciar_tokenizador(&t, "");
    CHECK(t.posicao == 0, "posição inicial deve ser zero");
    CHECK(t.linha == 1, "linha inicial deve ser um");
    CHECK(t.coluna == 1, "coluna inicial deve ser um");
    verificar_token(&t, TOKEN_FIM, "", 1, 1, "entrada vazia deve produzir FIM");
    verificar_token(&t, TOKEN_FIM, "", 1, 1, "FIM repetido permanece no fim");
}

static void testar_palavras_reservadas(void)
{
    Tokenizador t;
    iniciar_tokenizador(&t, "CREATE TABLE INSERT INTO VALUES SELECT FROM WHERE AND INT CHAR");
    verificar_token(&t, TOKEN_CREATE, "CREATE", 1, 1, "CREATE");
    verificar_token(&t, TOKEN_ESPACO, " ", 1, 7, "espaço");
    verificar_token(&t, TOKEN_TABLE, "TABLE", 1, 8, "TABLE");
    verificar_token(&t, TOKEN_ESPACO, " ", 1, 13, "espaço");
    verificar_token(&t, TOKEN_INSERT, "INSERT", 1, 14, "INSERT");
    verificar_token(&t, TOKEN_ESPACO, " ", 1, 20, "espaço");
    verificar_token(&t, TOKEN_INTO, "INTO", 1, 21, "INTO");
    verificar_token(&t, TOKEN_ESPACO, " ", 1, 25, "espaço");
    verificar_token(&t, TOKEN_VALUES, "VALUES", 1, 26, "VALUES");
    verificar_token(&t, TOKEN_ESPACO, " ", 1, 32, "espaço");
    verificar_token(&t, TOKEN_SELECT, "SELECT", 1, 33, "SELECT");
    verificar_token(&t, TOKEN_ESPACO, " ", 1, 39, "espaço");
    verificar_token(&t, TOKEN_FROM, "FROM", 1, 40, "FROM");
    verificar_token(&t, TOKEN_ESPACO, " ", 1, 44, "espaço");
    verificar_token(&t, TOKEN_WHERE, "WHERE", 1, 45, "WHERE");
    verificar_token(&t, TOKEN_ESPACO, " ", 1, 50, "espaço");
    verificar_token(&t, TOKEN_AND, "AND", 1, 51, "AND");
    verificar_token(&t, TOKEN_ESPACO, " ", 1, 54, "espaço");
    verificar_token(&t, TOKEN_INT, "INT", 1, 55, "INT");
    verificar_token(&t, TOKEN_ESPACO, " ", 1, 58, "espaço");
    verificar_token(&t, TOKEN_CHAR, "CHAR", 1, 59, "CHAR");
    verificar_token(&t, TOKEN_FIM, "", 1, 63, "fim após palavras reservadas");
}

static void testar_palavras_case_insensitive(void)
{
    Tokenizador t;
    iniciar_tokenizador(&t, "select Select sElEcT");
    verificar_token(&t, TOKEN_SELECT, "select", 1, 1, "select minúsculo");
    verificar_token(&t, TOKEN_ESPACO, " ", 1, 7, "espaço");
    verificar_token(&t, TOKEN_SELECT, "Select", 1, 8, "Select misto");
    verificar_token(&t, TOKEN_ESPACO, " ", 1, 14, "espaço");
    verificar_token(&t, TOKEN_SELECT, "sElEcT", 1, 15, "SELECT misto");
}

static void testar_identificadores(void)
{
    Tokenizador t;
    iniciar_tokenizador(&t, "alunos _id aluno123 A1 _");
    verificar_token(&t, TOKEN_IDENTACAO, "alunos", 1, 1, "identificador simples");
    verificar_token(&t, TOKEN_ESPACO, " ", 1, 7, "espaço");
    verificar_token(&t, TOKEN_IDENTACAO, "_id", 1, 8, "identificador iniciado por sublinhado");
    verificar_token(&t, TOKEN_ESPACO, " ", 1, 11, "espaço");
    verificar_token(&t, TOKEN_IDENTACAO, "aluno123", 1, 12, "identificador com dígitos");
    verificar_token(&t, TOKEN_ESPACO, " ", 1, 20, "espaço");
    verificar_token(&t, TOKEN_IDENTACAO, "A1", 1, 21, "identificador alfanumérico");
    verificar_token(&t, TOKEN_ESPACO, " ", 1, 23, "espaço");
    verificar_token(&t, TOKEN_IDENTACAO, "_", 1, 24, "identificador sublinhado");
}

static void testar_simbolos(void)
{
    Tokenizador t;
    iniciar_tokenizador(&t, "(),;*=");
    verificar_token(&t, TOKEN_ABRE_PARENTESE, "(", 1, 1, "(");
    verificar_token(&t, TOKEN_FECHA_PARENTESE, ")", 1, 2, ")");
    verificar_token(&t, TOKEN_VIRGULA, ",", 1, 3, "vírgula");
    verificar_token(&t, TOKEN_PONTO_VIRGULA, ";", 1, 4, "ponto e vírgula");
    verificar_token(&t, TOKEN_ASTERISCO, "*", 1, 5, "asterisco");
    verificar_token(&t, TOKEN_IGUAL, "=", 1, 6, "igual");
}

static void testar_operadores(void)
{
    Tokenizador t;
    iniciar_tokenizador(&t, "< <= <> > >= != !");
    verificar_token(&t, TOKEN_MENOR, "<", 1, 1, "menor");
    verificar_token(&t, TOKEN_ESPACO, " ", 1, 2, "espaço");
    verificar_token(&t, TOKEN_MENOR_IGUAL, "<=", 1, 3, "menor ou igual");
    verificar_token(&t, TOKEN_ESPACO, " ", 1, 5, "espaço");
    verificar_token(&t, TOKEN_DIFERENTE, "<>", 1, 6, "diferente SQL");
    verificar_token(&t, TOKEN_ESPACO, " ", 1, 8, "espaço");
    verificar_token(&t, TOKEN_MAIOR, ">", 1, 9, "maior");
    verificar_token(&t, TOKEN_ESPACO, " ", 1, 10, "espaço");
    verificar_token(&t, TOKEN_MAIOR_IGUAL, ">=", 1, 11, "maior ou igual");
    verificar_token(&t, TOKEN_ESPACO, " ", 1, 13, "espaço");
    verificar_token(&t, TOKEN_DIFERENTE, "!=", 1, 14, "diferente alternativo");
    verificar_token(&t, TOKEN_ESPACO, " ", 1, 16, "espaço");
    verificar_token(&t, TOKEN_ERRO, "!", 1, 17, "exclamação isolada é erro");
}

static void testar_numeros(void)
{
    Tokenizador t;
    iniciar_tokenizador(&t, "0 123 999999");
    verificar_token(&t, TOKEN_NUMERO, "0", 1, 1, "zero");
    verificar_token(&t, TOKEN_ESPACO, " ", 1, 2, "espaço");
    verificar_token(&t, TOKEN_NUMERO, "123", 1, 3, "inteiro positivo");
    verificar_token(&t, TOKEN_ESPACO, " ", 1, 6, "espaço");
    verificar_token(&t, TOKEN_NUMERO, "999999", 1, 7, "número maior");
}

static void testar_strings(void)
{
    Tokenizador t;
    iniciar_tokenizador(&t, "'' 'Ana' '12345678' '123456789' 'sem fechamento");
    verificar_token(&t, TOKEN_STRING, "", 1, 1, "string vazia");
    verificar_token(&t, TOKEN_ESPACO, " ", 1, 3, "espaço");
    verificar_token(&t, TOKEN_STRING, "Ana", 1, 4, "string curta");
    verificar_token(&t, TOKEN_ESPACO, " ", 1, 9, "espaço");
    verificar_token(&t, TOKEN_STRING, "12345678", 1, 10, "string com 8 caracteres");
    verificar_token(&t, TOKEN_ESPACO, " ", 1, 20, "espaço");
    verificar_token(&t, TOKEN_ERRO, "123456789", 1, 21, "string com 9 caracteres deve ser erro");
    verificar_token(&t, TOKEN_ESPACO, " ", 1, 32, "espaço");
    verificar_token(&t, TOKEN_ERRO, "sem fechamento", 1, 33, "string sem aspas de fechamento");
}

static void testar_espacos_e_posicoes(void)
{
    Tokenizador t;
    iniciar_tokenizador(&t, "a \t \nb");
    verificar_token(&t, TOKEN_IDENTACAO, "a", 1, 1, "primeiro identificador");
    verificar_token(&t, TOKEN_ESPACO, " ", 1, 2, "sequência de espaços em branco");
    verificar_token(&t, TOKEN_IDENTACAO, "b", 2, 1, "identificador após quebra de linha");
}

static void testar_palavras_nao_suportadas(void)
{
    Tokenizador t;
    iniciar_tokenizador(&t, "OR JOIN INNER LEFT RIGHT ON ORDER BY GROUP HAVING COUNT SUM AVG MIN MAX");
    const char *palavras[] = {
        "OR", "JOIN", "INNER", "LEFT", "RIGHT", "ON", "ORDER", "BY",
        "GROUP", "HAVING", "COUNT", "SUM", "AVG", "MIN", "MAX"
    };
    for (size_t i = 0; i < sizeof palavras / sizeof palavras[0]; i++) {
        Token token = proximo_token(&t);
        CHECK(token.tipo == TOKEN_NAO_SUPORTADO, "palavra fora do escopo");
        CHECK(strcmp(token.lexema, palavras[i]) == 0, "lexema não suportado");
        if (i + 1 < sizeof palavras / sizeof palavras[0])
            (void)proximo_token(&t); /* token ESPACO */
    }
}

static void testar_entradas_invalidas(void)
{
    Tokenizador t;
    iniciar_tokenizador(&t, "@ 12abc abc$");
    verificar_token(&t, TOKEN_ERRO, "@", 1, 1, "caractere inválido");
    verificar_token(&t, TOKEN_ESPACO, " ", 1, 2, "espaço");
    verificar_token(&t, TOKEN_ERRO, "12abc", 1, 3, "número colado em letras");
    verificar_token(&t, TOKEN_ESPACO, " ", 1, 8, "espaço");
    verificar_token(&t, TOKEN_IDENTACAO, "abc", 1, 9, "identificador antes de caractere inválido");
    verificar_token(&t, TOKEN_ERRO, "$", 1, 12, "caractere inválido após identificador");
}

static void testar_identificador_longo(void)
{
    char entrada[100];
    memset(entrada, 'a', 70);
    entrada[70] = '\0';

    Tokenizador t;
    iniciar_tokenizador(&t, entrada);
    Token token = proximo_token(&t);
    CHECK(token.tipo == TOKEN_ERRO, "identificador maior que MAX_LEXEMA deve ser erro");
    CHECK(strlen(token.lexema) == MAX_LEXEMA - 1, "lexema deve ser truncado ao limite do buffer");
}

static void testar_comando_completo(void)
{
    const char *sql =
        "SELECT * FROM alunos WHERE id >= 1 AND nome <> 'Bia';";
    Tokenizador t;
    iniciar_tokenizador(&t, sql);

    const TipoToken esperados[] = {
        TOKEN_SELECT, TOKEN_ESPACO, TOKEN_ASTERISCO, TOKEN_ESPACO,
        TOKEN_FROM, TOKEN_ESPACO, TOKEN_IDENTACAO, TOKEN_ESPACO,
        TOKEN_WHERE, TOKEN_ESPACO, TOKEN_IDENTACAO, TOKEN_ESPACO,
        TOKEN_MAIOR_IGUAL, TOKEN_ESPACO, TOKEN_NUMERO, TOKEN_ESPACO,
        TOKEN_AND, TOKEN_ESPACO, TOKEN_IDENTACAO, TOKEN_ESPACO,
        TOKEN_DIFERENTE, TOKEN_ESPACO, TOKEN_STRING, TOKEN_PONTO_VIRGULA,
        TOKEN_FIM
    };

    for (size_t i = 0; i < sizeof esperados / sizeof esperados[0]; i++) {
        Token token = proximo_token(&t);
        CHECK(token.tipo == esperados[i], "sequência de tokens do SELECT completo");
    }
}

int main(void)
{
    testar_inicializacao_e_fim();
    testar_palavras_reservadas();
    testar_palavras_case_insensitive();
    testar_identificadores();
    testar_simbolos();
    testar_operadores();
    testar_numeros();
    testar_strings();
    testar_espacos_e_posicoes();
    testar_palavras_nao_suportadas();
    testar_entradas_invalidas();
    testar_identificador_longo();
    testar_comando_completo();

    printf("\nTestes executados: %d\n", total_testes);
    printf("Testes com falha:  %d\n", testes_falhos);

    return testes_falhos == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
