#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "../../modules/parser/tokenizer.c"

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

/*
 * Verifica a sequencia de tokens de uma entrada, ignorando TOKEN_ESPACO.
 * Termina conferindo que o proximo token e TOKEN_FIM.
 */
static void verificar_sequencia(const char *sql,
                                const TipoToken *tipos,
                                const char *const *lexemas,
                                size_t quantidade,
                                const char *descricao)
{
    Tokenizador t;
    iniciar_tokenizador(&t, sql);

    for (size_t i = 0; i < quantidade; i++) {
        Token token = proximo_token(&t);
        while (token.tipo == TOKEN_ESPACO)
            token = proximo_token(&t);
        CHECK(token.tipo == tipos[i], descricao);
        CHECK(strcmp(token.lexema, lexemas[i]) == 0, "lexema inesperado na sequência");
    }

    Token fim = proximo_token(&t);
    while (fim.tipo == TOKEN_ESPACO)
        fim = proximo_token(&t);
    CHECK(fim.tipo == TOKEN_FIM, "sequência deve terminar em FIM");
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
    /* "ON" nao entra aqui: e palavra reservada suportada (CREATE INDEX ... ON) */
    Tokenizador t;
    iniciar_tokenizador(&t, "OR JOIN INNER LEFT RIGHT ORDER BY GROUP HAVING COUNT SUM AVG MIN MAX");
    const char *palavras[] = {
        "OR", "JOIN", "INNER", "LEFT", "RIGHT", "ORDER", "BY",
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

/* ------------------------------------------------------------------ */
/* Testes de CREATE INDEX                                              */
/* ------------------------------------------------------------------ */

static void testar_palavras_index_on(void)
{
    Tokenizador t;
    iniciar_tokenizador(&t, "INDEX ON index on Index On iNdEx oN");
    verificar_token(&t, TOKEN_INDEX, "INDEX", 1, 1, "INDEX maiúsculo");
    verificar_token(&t, TOKEN_ESPACO, " ", 1, 6, "espaço");
    verificar_token(&t, TOKEN_ON, "ON", 1, 7, "ON maiúsculo");
    verificar_token(&t, TOKEN_ESPACO, " ", 1, 9, "espaço");
    verificar_token(&t, TOKEN_INDEX, "index", 1, 10, "index minúsculo");
    verificar_token(&t, TOKEN_ESPACO, " ", 1, 15, "espaço");
    verificar_token(&t, TOKEN_ON, "on", 1, 16, "on minúsculo");
    verificar_token(&t, TOKEN_ESPACO, " ", 1, 18, "espaço");
    verificar_token(&t, TOKEN_INDEX, "Index", 1, 19, "Index misto");
    verificar_token(&t, TOKEN_ESPACO, " ", 1, 24, "espaço");
    verificar_token(&t, TOKEN_ON, "On", 1, 25, "On misto");
    verificar_token(&t, TOKEN_ESPACO, " ", 1, 27, "espaço");
    verificar_token(&t, TOKEN_INDEX, "iNdEx", 1, 28, "iNdEx misto");
    verificar_token(&t, TOKEN_ESPACO, " ", 1, 33, "espaço");
    verificar_token(&t, TOKEN_ON, "oN", 1, 34, "oN misto");
    verificar_token(&t, TOKEN_FIM, "", 1, 36, "fim após INDEX/ON");
}

static void testar_index_on_nao_sao_nao_suportados(void)
{
    Tokenizador t;
    iniciar_tokenizador(&t, "INDEX ON");
    Token token = proximo_token(&t);
    CHECK(token.tipo != TOKEN_NAO_SUPORTADO, "INDEX não pode ser NAO_SUPORTADO");
    CHECK(token.tipo != TOKEN_IDENTACAO, "INDEX não pode ser identificador");
    (void)proximo_token(&t); /* ESPACO */
    token = proximo_token(&t);
    CHECK(token.tipo != TOKEN_NAO_SUPORTADO, "ON não pode ser NAO_SUPORTADO");
    CHECK(token.tipo != TOKEN_IDENTACAO, "ON não pode ser identificador");
}

static void testar_prefixos_de_index_on_sao_identificadores(void)
{
    /* palavras que apenas comecam com INDEX/ON nao sao reservadas */
    Tokenizador t;
    iniciar_tokenizador(&t, "online index1 indexes _on on_x idx");
    verificar_token(&t, TOKEN_IDENTACAO, "online", 1, 1, "online é identificador");
    verificar_token(&t, TOKEN_ESPACO, " ", 1, 7, "espaço");
    verificar_token(&t, TOKEN_IDENTACAO, "index1", 1, 8, "index1 é identificador");
    verificar_token(&t, TOKEN_ESPACO, " ", 1, 14, "espaço");
    verificar_token(&t, TOKEN_IDENTACAO, "indexes", 1, 15, "indexes é identificador");
    verificar_token(&t, TOKEN_ESPACO, " ", 1, 22, "espaço");
    verificar_token(&t, TOKEN_IDENTACAO, "_on", 1, 23, "_on é identificador");
    verificar_token(&t, TOKEN_ESPACO, " ", 1, 26, "espaço");
    verificar_token(&t, TOKEN_IDENTACAO, "on_x", 1, 27, "on_x é identificador");
    verificar_token(&t, TOKEN_ESPACO, " ", 1, 31, "espaço");
    verificar_token(&t, TOKEN_IDENTACAO, "idx", 1, 32, "idx é identificador");
}

static void testar_create_index_posicoes(void)
{
    Tokenizador t;
    iniciar_tokenizador(&t, "CREATE INDEX idx_nome ON alunos (nome);");
    verificar_token(&t, TOKEN_CREATE, "CREATE", 1, 1, "CREATE");
    verificar_token(&t, TOKEN_ESPACO, " ", 1, 7, "espaço");
    verificar_token(&t, TOKEN_INDEX, "INDEX", 1, 8, "INDEX");
    verificar_token(&t, TOKEN_ESPACO, " ", 1, 13, "espaço");
    verificar_token(&t, TOKEN_IDENTACAO, "idx_nome", 1, 14, "nome do índice");
    verificar_token(&t, TOKEN_ESPACO, " ", 1, 22, "espaço");
    verificar_token(&t, TOKEN_ON, "ON", 1, 23, "ON");
    verificar_token(&t, TOKEN_ESPACO, " ", 1, 25, "espaço");
    verificar_token(&t, TOKEN_IDENTACAO, "alunos", 1, 26, "nome da tabela");
    verificar_token(&t, TOKEN_ESPACO, " ", 1, 32, "espaço");
    verificar_token(&t, TOKEN_ABRE_PARENTESE, "(", 1, 33, "abre parêntese");
    verificar_token(&t, TOKEN_IDENTACAO, "nome", 1, 34, "nome da coluna");
    verificar_token(&t, TOKEN_FECHA_PARENTESE, ")", 1, 38, "fecha parêntese");
    verificar_token(&t, TOKEN_PONTO_VIRGULA, ";", 1, 39, "ponto e vírgula");
    verificar_token(&t, TOKEN_FIM, "", 1, 40, "fim do CREATE INDEX");
}

static void testar_create_index_case_insensitive(void)
{
    const TipoToken tipos[] = {
        TOKEN_CREATE, TOKEN_INDEX, TOKEN_IDENTACAO, TOKEN_ON,
        TOKEN_IDENTACAO, TOKEN_ABRE_PARENTESE, TOKEN_IDENTACAO,
        TOKEN_FECHA_PARENTESE, TOKEN_PONTO_VIRGULA
    };
    const char *const lexemas[] = {
        "create", "index", "idx", "on", "t", "(", "c", ")", ";"
    };
    verificar_sequencia("create index idx on t (c);", tipos, lexemas,
                        sizeof tipos / sizeof tipos[0],
                        "CREATE INDEX em minúsculas");

    const char *const lexemas_mistos[] = {
        "Create", "iNdEx", "idx", "On", "t", "(", "c", ")", ";"
    };
    verificar_sequencia("Create iNdEx idx On t (c);", tipos, lexemas_mistos,
                        sizeof tipos / sizeof tipos[0],
                        "CREATE INDEX em caixa mista");
}

static void testar_create_index_sem_espacos_nos_parenteses(void)
{
    const TipoToken tipos[] = {
        TOKEN_CREATE, TOKEN_INDEX, TOKEN_IDENTACAO, TOKEN_ON,
        TOKEN_IDENTACAO, TOKEN_ABRE_PARENTESE, TOKEN_IDENTACAO,
        TOKEN_FECHA_PARENTESE, TOKEN_PONTO_VIRGULA
    };
    const char *const lexemas[] = {
        "CREATE", "INDEX", "idx", "ON", "t", "(", "c", ")", ";"
    };
    verificar_sequencia("CREATE INDEX idx ON t(c);", tipos, lexemas,
                        sizeof tipos / sizeof tipos[0],
                        "parênteses colados no nome da tabela");
}

static void testar_create_index_varias_linhas(void)
{
    Tokenizador t;
    iniciar_tokenizador(&t, "CREATE INDEX idx\nON alunos\n  (id);");
    verificar_token(&t, TOKEN_CREATE, "CREATE", 1, 1, "CREATE");
    verificar_token(&t, TOKEN_ESPACO, " ", 1, 7, "espaço");
    verificar_token(&t, TOKEN_INDEX, "INDEX", 1, 8, "INDEX");
    verificar_token(&t, TOKEN_ESPACO, " ", 1, 13, "espaço");
    verificar_token(&t, TOKEN_IDENTACAO, "idx", 1, 14, "nome do índice");
    verificar_token(&t, TOKEN_ESPACO, " ", 1, 17, "quebra de linha");
    verificar_token(&t, TOKEN_ON, "ON", 2, 1, "ON na segunda linha");
    verificar_token(&t, TOKEN_ESPACO, " ", 2, 3, "espaço");
    verificar_token(&t, TOKEN_IDENTACAO, "alunos", 2, 4, "tabela na segunda linha");
    verificar_token(&t, TOKEN_ESPACO, " ", 2, 10, "quebra de linha e indentação");
    verificar_token(&t, TOKEN_ABRE_PARENTESE, "(", 3, 3, "abre parêntese na terceira linha");
    verificar_token(&t, TOKEN_IDENTACAO, "id", 3, 4, "coluna na terceira linha");
    verificar_token(&t, TOKEN_FECHA_PARENTESE, ")", 3, 6, "fecha parêntese");
    verificar_token(&t, TOKEN_PONTO_VIRGULA, ";", 3, 7, "ponto e vírgula");
    verificar_token(&t, TOKEN_FIM, "", 3, 8, "fim na terceira linha");
}

static void testar_create_index_varias_colunas(void)
{
    /* o tokenizador aceita a lista; rejeitar mais de uma coluna e papel do parser */
    const TipoToken tipos[] = {
        TOKEN_CREATE, TOKEN_INDEX, TOKEN_IDENTACAO, TOKEN_ON,
        TOKEN_IDENTACAO, TOKEN_ABRE_PARENTESE, TOKEN_IDENTACAO,
        TOKEN_VIRGULA, TOKEN_IDENTACAO, TOKEN_FECHA_PARENTESE,
        TOKEN_PONTO_VIRGULA
    };
    const char *const lexemas[] = {
        "CREATE", "INDEX", "idx", "ON", "t", "(", "a", ",", "b", ")", ";"
    };
    verificar_sequencia("CREATE INDEX idx ON t (a, b);", tipos, lexemas,
                        sizeof tipos / sizeof tipos[0],
                        "CREATE INDEX com duas colunas");
}

static void testar_create_index_nome_reservado(void)
{
    /* um nome de indice igual a palavra reservada nao vira IDENT */
    const TipoToken tipos[] = {
        TOKEN_CREATE, TOKEN_INDEX, TOKEN_INDEX, TOKEN_ON,
        TOKEN_IDENTACAO, TOKEN_ABRE_PARENTESE, TOKEN_IDENTACAO,
        TOKEN_FECHA_PARENTESE, TOKEN_PONTO_VIRGULA
    };
    const char *const lexemas[] = {
        "CREATE", "INDEX", "index", "ON", "t", "(", "c", ")", ";"
    };
    verificar_sequencia("CREATE INDEX index ON t (c);", tipos, lexemas,
                        sizeof tipos / sizeof tipos[0],
                        "nome de índice igual a palavra reservada");

    const TipoToken tipos_tabela[] = {
        TOKEN_CREATE, TOKEN_INDEX, TOKEN_IDENTACAO, TOKEN_ON,
        TOKEN_ON, TOKEN_ABRE_PARENTESE, TOKEN_IDENTACAO,
        TOKEN_FECHA_PARENTESE, TOKEN_PONTO_VIRGULA
    };
    const char *const lexemas_tabela[] = {
        "CREATE", "INDEX", "idx", "ON", "on", "(", "c", ")", ";"
    };
    verificar_sequencia("CREATE INDEX idx ON on (c);", tipos_tabela, lexemas_tabela,
                        sizeof tipos_tabela / sizeof tipos_tabela[0],
                        "nome de tabela igual a palavra reservada");
}

static void testar_create_index_nomes_invalidos(void)
{
    /* nome iniciado por digito */
    Tokenizador t;
    iniciar_tokenizador(&t, "CREATE INDEX 1idx ON t (c);");
    verificar_token(&t, TOKEN_CREATE, "CREATE", 1, 1, "CREATE");
    verificar_token(&t, TOKEN_ESPACO, " ", 1, 7, "espaço");
    verificar_token(&t, TOKEN_INDEX, "INDEX", 1, 8, "INDEX");
    verificar_token(&t, TOKEN_ESPACO, " ", 1, 13, "espaço");
    verificar_token(&t, TOKEN_ERRO, "1idx", 1, 14, "nome de índice iniciado por dígito");

    /* hifen no nome do indice: IDENT, ERRO, IDENT */
    iniciar_tokenizador(&t, "CREATE INDEX idx-nome ON t (c);");
    verificar_token(&t, TOKEN_CREATE, "CREATE", 1, 1, "CREATE");
    verificar_token(&t, TOKEN_ESPACO, " ", 1, 7, "espaço");
    verificar_token(&t, TOKEN_INDEX, "INDEX", 1, 8, "INDEX");
    verificar_token(&t, TOKEN_ESPACO, " ", 1, 13, "espaço");
    verificar_token(&t, TOKEN_IDENTACAO, "idx", 1, 14, "início do nome com hífen");
    verificar_token(&t, TOKEN_ERRO, "-", 1, 17, "hífen é caractere inválido");
    verificar_token(&t, TOKEN_IDENTACAO, "nome", 1, 18, "resto do nome após hífen");

    /* ponto no nome (schema.tabela nao e suportado) */
    iniciar_tokenizador(&t, "CREATE INDEX idx ON db.t (c);");
    for (int i = 0; i < 7; i++)
        (void)proximo_token(&t); /* CREATE ' ' INDEX ' ' idx ' ' ON */
    (void)proximo_token(&t);     /* espaço */
    verificar_token(&t, TOKEN_IDENTACAO, "db", 1, 23, "prefixo antes do ponto");
    verificar_token(&t, TOKEN_ERRO, ".", 1, 25, "ponto é caractere inválido");
    verificar_token(&t, TOKEN_IDENTACAO, "t", 1, 26, "tabela após o ponto");
}

static void testar_create_index_nome_longo(void)
{
    /* nome de indice com 70 caracteres estoura MAX_LEXEMA */
    char sql[200];
    char nome[71];
    memset(nome, 'i', 70);
    nome[70] = '\0';
    snprintf(sql, sizeof sql, "CREATE INDEX %s ON t (c);", nome);

    Tokenizador t;
    iniciar_tokenizador(&t, sql);
    (void)proximo_token(&t); /* CREATE */
    (void)proximo_token(&t); /* espaço */
    (void)proximo_token(&t); /* INDEX  */
    (void)proximo_token(&t); /* espaço */
    Token token = proximo_token(&t);
    CHECK(token.tipo == TOKEN_ERRO, "nome de índice maior que MAX_LEXEMA deve ser erro");
    CHECK(strlen(token.lexema) == MAX_LEXEMA - 1, "lexema do índice truncado ao limite");

    /* nome com exatamente MAX_LEXEMA - 1 caracteres e valido */
    char nome_limite[MAX_LEXEMA];
    memset(nome_limite, 'i', MAX_LEXEMA - 1);
    nome_limite[MAX_LEXEMA - 1] = '\0';
    snprintf(sql, sizeof sql, "CREATE INDEX %s ON t (c);", nome_limite);

    iniciar_tokenizador(&t, sql);
    (void)proximo_token(&t);
    (void)proximo_token(&t);
    (void)proximo_token(&t);
    (void)proximo_token(&t);
    token = proximo_token(&t);
    CHECK(token.tipo == TOKEN_IDENTACAO, "nome de índice no limite deve ser identificador");
    CHECK(strlen(token.lexema) == MAX_LEXEMA - 1, "lexema no limite preservado");
}

static void testar_create_index_nao_suportado(void)
{
    /* UNIQUE nao e reservada (vira IDENT); USING e ASC/DESC tambem; o parser decide.
     * Aqui garantimos so que palavras fora do escopo continuam sinalizadas. */
    const TipoToken tipos[] = {
        TOKEN_CREATE, TOKEN_INDEX, TOKEN_IDENTACAO, TOKEN_ON,
        TOKEN_IDENTACAO, TOKEN_ABRE_PARENTESE, TOKEN_IDENTACAO,
        TOKEN_FECHA_PARENTESE, TOKEN_NAO_SUPORTADO, TOKEN_IDENTACAO,
        TOKEN_PONTO_VIRGULA
    };
    const char *const lexemas[] = {
        "CREATE", "INDEX", "idx", "ON", "t", "(", "c", ")", "ORDER", "x", ";"
    };
    verificar_sequencia("CREATE INDEX idx ON t (c) ORDER x;", tipos, lexemas,
                        sizeof tipos / sizeof tipos[0],
                        "palavra não suportada após CREATE INDEX");
}

static void testar_create_table_e_index_em_sequencia(void)
{
    const char *sql =
        "CREATE TABLE t (id INT, nome CHAR);\n"
        "CREATE INDEX idx ON t (id);";
    Tokenizador t;
    iniciar_tokenizador(&t, sql);

    const TipoToken esperados[] = {
        TOKEN_CREATE, TOKEN_TABLE, TOKEN_IDENTACAO, TOKEN_ABRE_PARENTESE,
        TOKEN_IDENTACAO, TOKEN_INT, TOKEN_VIRGULA, TOKEN_IDENTACAO,
        TOKEN_CHAR, TOKEN_FECHA_PARENTESE, TOKEN_PONTO_VIRGULA,
        TOKEN_CREATE, TOKEN_INDEX, TOKEN_IDENTACAO, TOKEN_ON,
        TOKEN_IDENTACAO, TOKEN_ABRE_PARENTESE, TOKEN_IDENTACAO,
        TOKEN_FECHA_PARENTESE, TOKEN_PONTO_VIRGULA, TOKEN_FIM
    };

    int linha_do_segundo_create = 0;
    int create_vistos = 0;
    for (size_t i = 0; i < sizeof esperados / sizeof esperados[0]; i++) {
        Token token = proximo_token(&t);
        while (token.tipo == TOKEN_ESPACO)
            token = proximo_token(&t);
        CHECK(token.tipo == esperados[i], "sequência CREATE TABLE seguida de CREATE INDEX");
        if (token.tipo == TOKEN_CREATE && ++create_vistos == 2)
            linha_do_segundo_create = token.linha;
    }
    CHECK(linha_do_segundo_create == 2, "segundo CREATE deve estar na linha 2");
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

    /* CREATE INDEX */
    testar_palavras_index_on();
    testar_index_on_nao_sao_nao_suportados();
    testar_prefixos_de_index_on_sao_identificadores();
    testar_create_index_posicoes();
    testar_create_index_case_insensitive();
    testar_create_index_sem_espacos_nos_parenteses();
    testar_create_index_varias_linhas();
    testar_create_index_varias_colunas();
    testar_create_index_nome_reservado();
    testar_create_index_nomes_invalidos();
    testar_create_index_nome_longo();
    testar_create_index_nao_suportado();
    testar_create_table_e_index_em_sequencia();

    printf("\nTestes executados: %d\n", total_testes);
    printf("Testes com falha:  %d\n", testes_falhos);

    return testes_falhos == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
