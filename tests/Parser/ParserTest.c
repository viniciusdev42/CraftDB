/*
 * Testes unitarios do Analisador Sintatico (Parser.c).
 *
 * O arquivo Parser.c e incluido diretamente para permitir testar tambem as funcoes
 * "static" (consumir_token, sincronizar, falha, e_operador, ...).
 * O Parser.c, por sua vez, inclui o Tokenizer.c.
 *
 * Compilar e executar:
 *   gcc -std=c99 -Wall -Wextra -o ParserTest ParserTest.c
 *   ./ParserTest
 *
 * Codigo de saida: 0 se todos os testes passaram, 1 caso contrario.
 *
 */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L   /* dup, dup2, fileno, tmpfile */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#define T_DUP    _dup
#define T_DUP2   _dup2
#define T_CLOSE  _close
#define T_FILENO _fileno
#else
#include <unistd.h>
#define T_DUP    dup
#define T_DUP2   dup2
#define T_CLOSE  close
#define T_FILENO fileno
#endif

#define PARSER_TESTE
#include "Parser.c"

/* ================================================================== */
/* Mini-framework de testes                                            */
/* ================================================================== */

static int g_testes = 0;
static int g_testes_ok = 0;
static int g_falhas = 0;
static int g_verificacoes = 0;

#define VERIFICAR(cond) do {                                                   \
    g_verificacoes++;                                                          \
    if (!(cond)) {                                                             \
        g_falhas++;                                                            \
        printf("    falha em %s:%d -> %s\n", __FILE__, __LINE__, #cond);       \
    }                                                                          \
} while (0)

#define VERIFICAR_INT(esperado, obtido) do {                                   \
    long e_ = (long)(esperado), o_ = (long)(obtido);                           \
    g_verificacoes++;                                                          \
    if (e_ != o_) {                                                            \
        g_falhas++;                                                            \
        printf("    falha em %s:%d -> esperado %ld, obtido %ld (%s)\n",        \
               __FILE__, __LINE__, e_, o_, #obtido);                           \
    }                                                                          \
} while (0)

#define VERIFICAR_STR(esperado, obtido) do {                                   \
    const char *e_ = (esperado), *o_ = (obtido);                               \
    g_verificacoes++;                                                          \
    if (strcmp(e_, o_) != 0) {                                                 \
        g_falhas++;                                                            \
        printf("    falha em %s:%d -> esperado \"%s\", obtido \"%s\"\n",       \
               __FILE__, __LINE__, e_, o_);                                    \
    }                                                                          \
} while (0)

#define VERIFICAR_CONTEM(texto, trecho) do {                                   \
    const char *t_ = (texto), *r_ = (trecho);                                  \
    g_verificacoes++;                                                          \
    if (strstr(t_, r_) == NULL) {                                              \
        g_falhas++;                                                            \
        printf("    falha em %s:%d -> \"%s\" nao contem \"%s\"\n",             \
               __FILE__, __LINE__, t_, r_);                                    \
    }                                                                          \
} while (0)

#define VERIFICAR_TOKEN(esperado, obtido) do {                                 \
    TipoToken e_ = (esperado), o_ = (obtido);                                  \
    g_verificacoes++;                                                          \
    if (e_ != o_) {                                                            \
        g_falhas++;                                                            \
        printf("    falha em %s:%d -> token esperado %s, obtido %s\n",         \
               __FILE__, __LINE__, nome_token(e_), nome_token(o_));            \
    }                                                                          \
} while (0)

#define EXECUTAR(teste) do {                                                   \
    int antes_ = g_falhas;                                                     \
    g_testes++;                                                                \
    teste();                                                                   \
    if (g_falhas == antes_) { g_testes_ok++; printf("[ OK ] %s\n", #teste); }  \
    else                    { printf("[FALHOU] %s\n", #teste); }               \
} while (0)

/* ================================================================== */
/* Funcoes auxiliares dos testes                                       */
/* ================================================================== */

/* Prepara o parser com o texto e tenta ler um comando */
static int analisar(Parser *p, Comando *c, const char *texto)
{
    iniciar_parser(p, texto);
    return parse_comando(p, c);
}

/* Exige que o texto seja um comando valido */
static int esperar_ok_(int linha, Parser *p, Comando *c, const char *texto)
{
    int r = analisar(p, c, texto);
    g_verificacoes++;
    if (!r) {
        g_falhas++;
        printf("    falha na linha %d: \"%s\" deveria ser valido\n"
               "      mensagem: %s\n", linha, texto, p->mensagem);
    }
    return r;
}
#define ESPERAR_OK(texto) esperar_ok_(__LINE__, &p, &c, texto)

/* Exige que o texto falhe e que a mensagem contenha o trecho */
static void esperar_erro_(int linha, const char *texto, const char *trecho)
{
    Parser p;
    Comando c;
    int r = analisar(&p, &c, texto);

    g_verificacoes++;
    if (r != 0) {
        g_falhas++;
        printf("    falha na linha %d: \"%s\" deveria falhar\n", linha, texto);
        return;
    }
    g_verificacoes++;
    if (strstr(p.mensagem, trecho) == NULL) {
        g_falhas++;
        printf("    falha na linha %d: \"%s\"\n"
               "      mensagem obtida : \"%s\"\n"
               "      deveria conter  : \"%s\"\n", linha, texto, p.mensagem, trecho);
    }
}
#define ESPERAR_ERRO(texto, trecho) esperar_erro_(__LINE__, texto, trecho)

/* Geradores de comandos grandes (testes de limites) */
static void gerar_create(char *buf, size_t tam, int n)
{
    size_t pos = (size_t)snprintf(buf, tam, "CREATE TABLE t (");
    for (int i = 0; i < n; i++)
        pos += (size_t)snprintf(buf + pos, tam - pos, "%sc%d INT", i ? ", " : "", i);
    snprintf(buf + pos, tam - pos, ");");
}

static void gerar_insert(char *buf, size_t tam, int n)
{
    size_t pos = (size_t)snprintf(buf, tam, "INSERT INTO t VALUES (");
    for (int i = 0; i < n; i++)
        pos += (size_t)snprintf(buf + pos, tam - pos, "%s%d", i ? ", " : "", i);
    snprintf(buf + pos, tam - pos, ");");
}

static void gerar_select(char *buf, size_t tam, int n)
{
    size_t pos = (size_t)snprintf(buf, tam, "SELECT * FROM t WHERE ");
    for (int i = 0; i < n; i++)
        pos += (size_t)snprintf(buf + pos, tam - pos, "%sc%d = %d", i ? " AND " : "", i, i);
    snprintf(buf + pos, tam - pos, ";");
}

/* Executa um "programa" (varios comandos) como o parser real faria:
 * em caso de erro, sincroniza e continua. */
static void executar_programa(const char *texto, int *ok, int *erros,
                              char *ultima_msg, size_t tam_msg)
{
    Parser p;
    Comando c;

    *ok = 0;
    *erros = 0;
    if (ultima_msg) ultima_msg[0] = '\0';

    iniciar_parser(&p, texto);
    while (p.atual.tipo != TOKEN_FIM) {
        if (parse_comando(&p, &c)) {
            (*ok)++;
        } else {
            (*erros)++;
            if (ultima_msg) {
                strncpy(ultima_msg, p.mensagem, tam_msg - 1);
                ultima_msg[tam_msg - 1] = '\0';
            }
            sincronizar(&p);
        }
    }
}

/* Captura de stdout (para testar imprimir_comando / imprimir_valor) */
static FILE *g_captura = NULL;
static int   g_stdout_salvo = -1;

static void captura_iniciar(void)
{
    fflush(stdout);
    g_captura = tmpfile();
    g_stdout_salvo = T_DUP(T_FILENO(stdout));
    T_DUP2(T_FILENO(g_captura), T_FILENO(stdout));
}

static void captura_finalizar(char *buf, size_t tam)
{
    fflush(stdout);
    T_DUP2(g_stdout_salvo, T_FILENO(stdout));
    T_CLOSE(g_stdout_salvo);
    rewind(g_captura);
    size_t n = fread(buf, 1, tam - 1, g_captura);
    buf[n] = '\0';
    fclose(g_captura);
    g_captura = NULL;
}

/* ================================================================== */
/* 1. Inicializacao do parser                                          */
/* ================================================================== */

static void teste_iniciar_parser_texto_vazio(void)
{
    Parser p;
    iniciar_parser(&p, "");
    VERIFICAR_TOKEN(TOKEN_FIM, p.atual.tipo);
    VERIFICAR_STR("", p.mensagem);
}

static void teste_iniciar_parser_le_primeiro_token(void)
{
    Parser p;
    iniciar_parser(&p, "SELECT");
    VERIFICAR_TOKEN(TOKEN_SELECT, p.atual.tipo);
    VERIFICAR_STR("SELECT", p.atual.lexema);
    VERIFICAR_INT(1, p.atual.linha);
    VERIFICAR_INT(1, p.atual.coluna);
}

static void teste_iniciar_parser_ignora_espacos_iniciais(void)
{
    Parser p;
    iniciar_parser(&p, "   \n\t  CREATE");
    VERIFICAR_TOKEN(TOKEN_CREATE, p.atual.tipo);
    VERIFICAR_INT(2, p.atual.linha);
    VERIFICAR_INT(4, p.atual.coluna);
}

static void teste_avancar_parser_nunca_para_em_espaco(void)
{
    Parser p;
    iniciar_parser(&p, "a  \n  b\t\tc");
    VERIFICAR_STR("a", p.atual.lexema);
    avancar_parser(&p);
    VERIFICAR_STR("b", p.atual.lexema);
    avancar_parser(&p);
    VERIFICAR_STR("c", p.atual.lexema);
    avancar_parser(&p);
    VERIFICAR_TOKEN(TOKEN_FIM, p.atual.tipo);
    avancar_parser(&p);                         /* continua em FIM */
    VERIFICAR_TOKEN(TOKEN_FIM, p.atual.tipo);
}

static void teste_iniciar_parser_limpa_mensagem_anterior(void)
{
    Parser p;
    iniciar_parser(&p, "x");
    strcpy(p.mensagem, "lixo");
    iniciar_parser(&p, "y");
    VERIFICAR_STR("", p.mensagem);
}

/* ================================================================== */
/* 2. Tratamento de erros: erro_formato, falha, consumir_token,        */
/*    sincronizar                                                      */
/* ================================================================== */

static void teste_erro_formato_prefixo_e_retorno(void)
{
    Parser p;
    Token t;
    int r;

    memset(&t, 0, sizeof t);
    t.linha = 12;
    t.coluna = 345;
    r = erro_formato(&p, &t, "valor %d e %s", 7, "ok");
    VERIFICAR_INT(0, r);
    VERIFICAR_STR("Linha 12, coluna 345: valor 7 e ok", p.mensagem);
}

static void teste_erro_formato_trunca_sem_estourar_buffer(void)
{
    Parser p;
    Token t;
    char longo[1000];
    int r;

    memset(&t, 0, sizeof t);
    t.linha = 3;
    t.coluna = 7;
    memset(longo, 'x', sizeof longo - 1);
    longo[sizeof longo - 1] = '\0';

    r = erro_formato(&p, &t, "%s", longo);
    VERIFICAR_INT(0, r);
    VERIFICAR_INT(MAX_MESSAGEM - 1, strlen(p.mensagem));
    VERIFICAR(strncmp(p.mensagem, "Linha 3, coluna 7: xxx", 22) == 0);
}

static void teste_falha_token_erro(void)
{
    Parser p;
    iniciar_parser(&p, "@");
    VERIFICAR_INT(0, falha(&p, "qualquer"));
    VERIFICAR_STR("Linha 1, coluna 1: token invalido '@'", p.mensagem);
}

static void teste_falha_token_nao_suportado(void)
{
    Parser p;
    iniciar_parser(&p, "JOIN");
    VERIFICAR_INT(0, falha(&p, "qualquer"));
    VERIFICAR_STR("Linha 1, coluna 1: construcao nao suportada 'JOIN'", p.mensagem);
}

static void teste_falha_fim_inesperado(void)
{
    Parser p;
    iniciar_parser(&p, "");
    VERIFICAR_INT(0, falha(&p, "algo"));
    VERIFICAR_STR("Linha 1, coluna 1: fim inesperado do texto (esperado: algo)",
                  p.mensagem);
}

static void teste_falha_token_comum(void)
{
    Parser p;
    iniciar_parser(&p, "abc");
    VERIFICAR_INT(0, falha(&p, "algo"));
    VERIFICAR_STR("Linha 1, coluna 1: encontrado 'abc' (esperado: algo)", p.mensagem);
}

static void teste_consumir_token_sucesso_avanca_e_devolve_token(void)
{
    Parser p;
    Token saida;
    iniciar_parser(&p, "foo (");

    VERIFICAR_INT(1, consumir_token(&p, TOKEN_IDENTACAO, "nome", &saida));
    VERIFICAR_STR("foo", saida.lexema);
    VERIFICAR_TOKEN(TOKEN_IDENTACAO, saida.tipo);
    VERIFICAR_TOKEN(TOKEN_ABRE_PARENTESE, p.atual.tipo);
    VERIFICAR_STR("", p.mensagem);
}

static void teste_consumir_token_saida_nula(void)
{
    Parser p;
    iniciar_parser(&p, "( )");
    VERIFICAR_INT(1, consumir_token(&p, TOKEN_ABRE_PARENTESE, "'('", NULL));
    VERIFICAR_TOKEN(TOKEN_FECHA_PARENTESE, p.atual.tipo);
}

static void teste_consumir_token_falha_nao_avanca(void)
{
    Parser p;
    Token saida;
    memset(&saida, 0, sizeof saida);
    iniciar_parser(&p, "( x");

    VERIFICAR_INT(0, consumir_token(&p, TOKEN_VIRGULA, "','", &saida));
    VERIFICAR_TOKEN(TOKEN_ABRE_PARENTESE, p.atual.tipo);   /* nao avancou */
    VERIFICAR_STR("", saida.lexema);                       /* saida intacta */
    VERIFICAR_STR("Linha 1, coluna 1: encontrado '(' (esperado: ',')", p.mensagem);
}

static void teste_sincronizar_consome_ate_depois_do_ponto_virgula(void)
{
    Parser p;
    iniciar_parser(&p, "a b c ; SELECT");
    sincronizar(&p);
    VERIFICAR_TOKEN(TOKEN_SELECT, p.atual.tipo);
}

static void teste_sincronizar_ja_em_ponto_virgula(void)
{
    Parser p;
    iniciar_parser(&p, "; SELECT");
    sincronizar(&p);
    VERIFICAR_TOKEN(TOKEN_SELECT, p.atual.tipo);
}

static void teste_sincronizar_sem_ponto_virgula_vai_ate_o_fim(void)
{
    Parser p;
    iniciar_parser(&p, "a b c");
    sincronizar(&p);
    VERIFICAR_TOKEN(TOKEN_FIM, p.atual.tipo);
}

static void teste_sincronizar_em_fim_nao_trava(void)
{
    Parser p;
    iniciar_parser(&p, "");
    sincronizar(&p);
    VERIFICAR_TOKEN(TOKEN_FIM, p.atual.tipo);
}

static void teste_sincronizar_so_pula_um_ponto_virgula(void)
{
    Parser p;
    iniciar_parser(&p, "; ;");
    sincronizar(&p);
    VERIFICAR_TOKEN(TOKEN_PONTO_VIRGULA, p.atual.tipo);
}

static void teste_sincronizar_pula_tokens_invalidos(void)
{
    Parser p;
    iniciar_parser(&p, "@ # 'aberta ; CREATE");
    /* a string sem fechar consome ate o fim do texto -> FIM */
    sincronizar(&p);
    VERIFICAR_TOKEN(TOKEN_FIM, p.atual.tipo);
}

/* ================================================================== */
/* 3. e_operador / simbolo_operacao                                    */
/* ================================================================== */

static void teste_e_operador_verdadeiros(void)
{
    VERIFICAR_INT(1, e_operador(TOKEN_IGUAL));
    VERIFICAR_INT(1, e_operador(TOKEN_DIFERENTE));
    VERIFICAR_INT(1, e_operador(TOKEN_MENOR));
    VERIFICAR_INT(1, e_operador(TOKEN_MENOR_IGUAL));
    VERIFICAR_INT(1, e_operador(TOKEN_MAIOR));
    VERIFICAR_INT(1, e_operador(TOKEN_MAIOR_IGUAL));
}

static void teste_e_operador_falsos(void)
{
    VERIFICAR_INT(0, e_operador(TOKEN_ASTERISCO));
    VERIFICAR_INT(0, e_operador(TOKEN_AND));
    VERIFICAR_INT(0, e_operador(TOKEN_NUMERO));
    VERIFICAR_INT(0, e_operador(TOKEN_STRING));
    VERIFICAR_INT(0, e_operador(TOKEN_IDENTACAO));
    VERIFICAR_INT(0, e_operador(TOKEN_VIRGULA));
    VERIFICAR_INT(0, e_operador(TOKEN_PONTO_VIRGULA));
    VERIFICAR_INT(0, e_operador(TOKEN_NAO_SUPORTADO));
    VERIFICAR_INT(0, e_operador(TOKEN_FIM));
    VERIFICAR_INT(0, e_operador(TOKEN_ERRO));
}

static void teste_simbolo_operacao(void)
{
    VERIFICAR_STR("=",  simbolo_operacao(TOKEN_IGUAL));
    VERIFICAR_STR("<>", simbolo_operacao(TOKEN_DIFERENTE));
    VERIFICAR_STR("<",  simbolo_operacao(TOKEN_MENOR));
    VERIFICAR_STR("<=", simbolo_operacao(TOKEN_MENOR_IGUAL));
    VERIFICAR_STR(">",  simbolo_operacao(TOKEN_MAIOR));
    VERIFICAR_STR(">=", simbolo_operacao(TOKEN_MAIOR_IGUAL));
    VERIFICAR_STR("?",  simbolo_operacao(TOKEN_AND));
}

/* ================================================================== */
/* 4. parse_valor                                                      */
/* ================================================================== */

static void teste_parse_valor_numero(void)
{
    Parser p;
    Valor v;
    memset(&v, 0xAA, sizeof v);
    iniciar_parser(&p, "42");

    VERIFICAR_INT(1, parse_valor(&p, &v));
    VERIFICAR_TOKEN(TOKEN_NUMERO, v.tipo);
    VERIFICAR_INT(42, v.numero);
    VERIFICAR_STR("", v.str);
    VERIFICAR_TOKEN(TOKEN_FIM, p.atual.tipo);
}

static void teste_parse_valor_string(void)
{
    Parser p;
    Valor v;
    memset(&v, 0xAA, sizeof v);
    iniciar_parser(&p, "'abc'");

    VERIFICAR_INT(1, parse_valor(&p, &v));
    VERIFICAR_TOKEN(TOKEN_STRING, v.tipo);
    VERIFICAR_INT(0, v.numero);
    VERIFICAR_STR("abc", v.str);
    VERIFICAR_TOKEN(TOKEN_FIM, p.atual.tipo);
}

static void teste_parse_valor_numero_zero_e_zeros_a_esquerda(void)
{
    Parser p;
    Valor v;

    iniciar_parser(&p, "0");
    VERIFICAR_INT(1, parse_valor(&p, &v));
    VERIFICAR_INT(0, v.numero);

    iniciar_parser(&p, "007");
    VERIFICAR_INT(1, parse_valor(&p, &v));
    VERIFICAR_INT(7, v.numero);
}

static void teste_parse_valor_limites_de_int(void)
{
    Parser p;
    Valor v;
    char msg_esperada[128];

    iniciar_parser(&p, "2147483647");                        /* INT_MAX */
    VERIFICAR_INT(1, parse_valor(&p, &v));
    VERIFICAR_INT(INT_MAX, v.numero);

    iniciar_parser(&p, "2147483648");                        /* INT_MAX + 1 */
    VERIFICAR_INT(0, parse_valor(&p, &v));
    snprintf(msg_esperada, sizeof msg_esperada,
             "numero '2147483648' fora do intervalo de INT");
    VERIFICAR_CONTEM(p.mensagem, msg_esperada);

    iniciar_parser(&p, "99999999999999999999999999");        /* ERANGE em long */
    VERIFICAR_INT(0, parse_valor(&p, &v));
    VERIFICAR_CONTEM(p.mensagem, "fora do intervalo de INT");
}

static void teste_parse_valor_invalido_nao_avanca(void)
{
    Parser p;
    Valor v;
    iniciar_parser(&p, "abc");

    VERIFICAR_INT(0, parse_valor(&p, &v));
    VERIFICAR_STR("Linha 1, coluna 1: encontrado 'abc' (esperado: numero ou string)",
                  p.mensagem);
    VERIFICAR_STR("abc", p.atual.lexema);
}

static void teste_parse_valor_rejeita_tokens_nao_valor(void)
{
    Parser p;
    Valor v;

    iniciar_parser(&p, ")");
    VERIFICAR_INT(0, parse_valor(&p, &v));
    iniciar_parser(&p, ",");
    VERIFICAR_INT(0, parse_valor(&p, &v));
    iniciar_parser(&p, "");
    VERIFICAR_INT(0, parse_valor(&p, &v));
    VERIFICAR_CONTEM(p.mensagem, "fim inesperado do texto");
    iniciar_parser(&p, "-5");
    VERIFICAR_INT(0, parse_valor(&p, &v));
    VERIFICAR_CONTEM(p.mensagem, "token invalido '-'");
}

/* ================================================================== */
/* 5. parse_condicao                                                   */
/* ================================================================== */

static void teste_parse_condicao_numero(void)
{
    Parser p;
    Condicao cond;
    memset(&cond, 0, sizeof cond);
    iniciar_parser(&p, "idade >= 18");

    VERIFICAR_INT(1, parse_condicao(&p, &cond));
    VERIFICAR_STR("idade", cond.coluna);
    VERIFICAR_TOKEN(TOKEN_MAIOR_IGUAL, cond.operacao);
    VERIFICAR_TOKEN(TOKEN_NUMERO, cond.valor.tipo);
    VERIFICAR_INT(18, cond.valor.numero);
    VERIFICAR_TOKEN(TOKEN_FIM, p.atual.tipo);
}

static void teste_parse_condicao_string(void)
{
    Parser p;
    Condicao cond;
    memset(&cond, 0, sizeof cond);
    iniciar_parser(&p, "nome <> 'Ana'");

    VERIFICAR_INT(1, parse_condicao(&p, &cond));
    VERIFICAR_STR("nome", cond.coluna);
    VERIFICAR_TOKEN(TOKEN_DIFERENTE, cond.operacao);
    VERIFICAR_TOKEN(TOKEN_STRING, cond.valor.tipo);
    VERIFICAR_STR("Ana", cond.valor.str);
}

static void teste_parse_condicao_erros(void)
{
    Parser p;
    Condicao cond;

    iniciar_parser(&p, "5 = 1");
    VERIFICAR_INT(0, parse_condicao(&p, &cond));
    VERIFICAR_CONTEM(p.mensagem, "esperado: nome da coluna");

    iniciar_parser(&p, "a 1");
    VERIFICAR_INT(0, parse_condicao(&p, &cond));
    VERIFICAR_CONTEM(p.mensagem, "esperado: operador");

    iniciar_parser(&p, "a =");
    VERIFICAR_INT(0, parse_condicao(&p, &cond));
    VERIFICAR_CONTEM(p.mensagem, "fim inesperado do texto (esperado: numero ou string)");

    iniciar_parser(&p, "a = b");
    VERIFICAR_INT(0, parse_condicao(&p, &cond));
    VERIFICAR_CONTEM(p.mensagem, "encontrado 'b' (esperado: numero ou string)");
}

/* ================================================================== */
/* 6. CREATE TABLE                                                     */
/* ================================================================== */

static void teste_create_uma_coluna(void)
{
    Parser p; Comando c;
    if (!ESPERAR_OK("CREATE TABLE t (id INT);")) return;

    VERIFICAR_INT(CMD_CREATE, c.tipo);
    VERIFICAR_STR("t", c.tabela);
    VERIFICAR_INT(1, c.numero_colunas);
    VERIFICAR_STR("id", c.colunas[0].nome);
    VERIFICAR_TOKEN(TOKEN_INT, c.colunas[0].tipo);
    VERIFICAR_INT(0, c.numero_valores);
    VERIFICAR_INT(0, c.numero_condicoes);
    VERIFICAR_TOKEN(TOKEN_FIM, p.atual.tipo);
}

static void teste_create_varias_colunas(void)
{
    Parser p; Comando c;
    if (!ESPERAR_OK("CREATE TABLE alunos (id INT, nome CHAR, idade INT);")) return;

    VERIFICAR_STR("alunos", c.tabela);
    VERIFICAR_INT(3, c.numero_colunas);
    VERIFICAR_STR("id", c.colunas[0].nome);
    VERIFICAR_TOKEN(TOKEN_INT, c.colunas[0].tipo);
    VERIFICAR_STR("nome", c.colunas[1].nome);
    VERIFICAR_TOKEN(TOKEN_CHAR, c.colunas[1].tipo);
    VERIFICAR_STR("idade", c.colunas[2].nome);
    VERIFICAR_TOKEN(TOKEN_INT, c.colunas[2].tipo);
}

static void teste_create_palavras_reservadas_sem_diferenciar_caixa(void)
{
    Parser p; Comando c;
    if (!ESPERAR_OK("create table Pessoas (Id int, Nome Char);")) return;

    VERIFICAR_STR("Pessoas", c.tabela);          /* caixa do nome e preservada */
    VERIFICAR_INT(2, c.numero_colunas);
    VERIFICAR_STR("Id", c.colunas[0].nome);
    VERIFICAR_TOKEN(TOKEN_INT, c.colunas[0].tipo);
    VERIFICAR_STR("Nome", c.colunas[1].nome);
    VERIFICAR_TOKEN(TOKEN_CHAR, c.colunas[1].tipo);
}

static void teste_create_sem_espacos_ao_redor_de_simbolos(void)
{
    Parser p; Comando c;
    if (!ESPERAR_OK("CREATE TABLE t(a INT,b CHAR);")) return;
    VERIFICAR_INT(2, c.numero_colunas);
    VERIFICAR_STR("b", c.colunas[1].nome);
}

static void teste_create_com_quebras_de_linha_e_tabs(void)
{
    Parser p; Comando c;
    if (!ESPERAR_OK("CREATE\n  TABLE\tt\n(\n  a INT ,\n  b CHAR\n)\n;")) return;
    VERIFICAR_STR("t", c.tabela);
    VERIFICAR_INT(2, c.numero_colunas);
}

static void teste_create_nome_com_underscore_e_digitos(void)
{
    Parser p; Comando c;
    if (!ESPERAR_OK("CREATE TABLE _tab_1 (col_2 INT);")) return;
    VERIFICAR_STR("_tab_1", c.tabela);
    VERIFICAR_STR("col_2", c.colunas[0].nome);
}

static void teste_create_limite_de_colunas(void)
{
    Parser p; Comando c;
    char buf[1024];

    gerar_create(buf, sizeof buf, MAX_COLUNAS);              /* 16: ok */
    if (ESPERAR_OK(buf)) {
        VERIFICAR_INT(MAX_COLUNAS, c.numero_colunas);
        VERIFICAR_STR("c15", c.colunas[MAX_COLUNAS - 1].nome);
    }

    gerar_create(buf, sizeof buf, MAX_COLUNAS + 1);          /* 17: erro */
    VERIFICAR_INT(0, analisar(&p, &c, buf));
    VERIFICAR_CONTEM(p.mensagem, "numero maximo de colunas (16) excedido");
}

static void teste_create_coluna_duplicada(void)
{
    ESPERAR_ERRO("CREATE TABLE t (a INT, a INT);", "coluna 'a' duplicada");
    /* comparacao sem diferenciar maiusculas/minusculas; posicao exata */
    ESPERAR_ERRO("CREATE TABLE t (a INT, A CHAR);",
                 "Linha 1, coluna 24: coluna 'A' duplicada");
    ESPERAR_ERRO("CREATE TABLE t (x INT, y INT, X INT);", "coluna 'X' duplicada");
}

static void teste_create_mesmo_nome_de_tabela_e_coluna_e_permitido(void)
{
    Parser p; Comando c;
    if (!ESPERAR_OK("CREATE TABLE t (t INT);")) return;
    VERIFICAR_STR("t", c.tabela);
    VERIFICAR_STR("t", c.colunas[0].nome);
}

static void teste_create_nome_no_limite_do_lexema(void)
{
    Parser p; Comando c;
    char nome63[MAX_LEXEMA], nome64[MAX_LEXEMA + 1], buf[256];

    memset(nome63, 'a', MAX_LEXEMA - 1);
    nome63[MAX_LEXEMA - 1] = '\0';                           /* 63 caracteres */
    snprintf(buf, sizeof buf, "CREATE TABLE %s (a INT);", nome63);
    if (ESPERAR_OK(buf)) {
        VERIFICAR_INT(MAX_LEXEMA - 1, strlen(c.tabela));
        VERIFICAR_STR(nome63, c.tabela);
    }

    memset(nome64, 'a', MAX_LEXEMA);
    nome64[MAX_LEXEMA] = '\0';                               /* 64 caracteres */
    snprintf(buf, sizeof buf, "CREATE TABLE %s (a INT);", nome64);
    VERIFICAR_INT(0, analisar(&p, &c, buf));
    VERIFICAR_CONTEM(p.mensagem, "token invalido");
}

static void teste_create_erros_de_sintaxe(void)
{
    ESPERAR_ERRO("CREATE",                      "fim inesperado do texto (esperado: TABLE)");
    ESPERAR_ERRO("CREATE t (a INT);",           "encontrado 't' (esperado: TABLE)");
    ESPERAR_ERRO("CREATE TABLE (a INT);",       "encontrado '(' (esperado: nome da tabela)");
    ESPERAR_ERRO("CREATE TABLE 123 (a INT);",   "encontrado '123' (esperado: nome da tabela)");
    ESPERAR_ERRO("CREATE TABLE select (a INT);","encontrado 'select' (esperado: nome da tabela)");
    ESPERAR_ERRO("CREATE TABLE t a INT);",      "encontrado 'a' (esperado: '(')");
    ESPERAR_ERRO("CREATE TABLE t ();",          "encontrado ')' (esperado: nome da coluna)");
    ESPERAR_ERRO("CREATE TABLE t (int INT);",   "encontrado 'int' (esperado: nome da coluna)");
    ESPERAR_ERRO("CREATE TABLE t (a);",         "encontrado ')' (esperado: INT ou CHAR)");
    ESPERAR_ERRO("CREATE TABLE t (a VARCHAR);", "encontrado 'VARCHAR' (esperado: INT ou CHAR)");
    ESPERAR_ERRO("CREATE TABLE t (a 'x');",     "esperado: INT ou CHAR");
    ESPERAR_ERRO("CREATE TABLE t (a INT b CHAR);", "encontrado 'b' (esperado: ')' ou ',')");
    ESPERAR_ERRO("CREATE TABLE t (a INT,);",    "encontrado ')' (esperado: nome da coluna)");
    ESPERAR_ERRO("CREATE TABLE t (a INT",       "fim inesperado do texto (esperado: ')' ou ',')");
    ESPERAR_ERRO("CREATE TABLE t (a INT)",      "fim inesperado do texto (esperado: ';')");
    ESPERAR_ERRO("CREATE TABLE t (a INT) x",    "encontrado 'x' (esperado: ';')");
}

static void teste_create_tokens_invalidos_e_nao_suportados(void)
{
    ESPERAR_ERRO("CREATE TABLE t (a INT, b @);",  "token invalido '@'");
    ESPERAR_ERRO("CREATE TABLE 1abc (a INT);",    "token invalido '1abc'");
    ESPERAR_ERRO("CREATE TABLE JOIN (a INT);",    "construcao nao suportada 'JOIN'");
    ESPERAR_ERRO("CREATE TABLE t (count INT);",   "construcao nao suportada 'count'");
}

/* ================================================================== */
/* 7. INSERT                                                           */
/* ================================================================== */

static void teste_insert_numeros(void)
{
    Parser p; Comando c;
    if (!ESPERAR_OK("INSERT INTO t VALUES (1, 22, 333);")) return;

    VERIFICAR_INT(CMD_INSERT, c.tipo);
    VERIFICAR_STR("t", c.tabela);
    VERIFICAR_INT(3, c.numero_valores);
    VERIFICAR_TOKEN(TOKEN_NUMERO, c.valores[0].tipo);
    VERIFICAR_INT(1,   c.valores[0].numero);
    VERIFICAR_INT(22,  c.valores[1].numero);
    VERIFICAR_INT(333, c.valores[2].numero);
    VERIFICAR_INT(0, c.numero_colunas);
    VERIFICAR_INT(0, c.numero_condicoes);
    VERIFICAR_TOKEN(TOKEN_FIM, p.atual.tipo);
}

static void teste_insert_strings(void)
{
    Parser p; Comando c;
    if (!ESPERAR_OK("INSERT INTO t VALUES ('abc', 'xyz');")) return;

    VERIFICAR_INT(2, c.numero_valores);
    VERIFICAR_TOKEN(TOKEN_STRING, c.valores[0].tipo);
    VERIFICAR_STR("abc", c.valores[0].str);
    VERIFICAR_STR("xyz", c.valores[1].str);
}

static void teste_insert_valores_mistos(void)
{
    Parser p; Comando c;
    if (!ESPERAR_OK("INSERT INTO alunos VALUES (10, 'Ana', 20);")) return;

    VERIFICAR_STR("alunos", c.tabela);
    VERIFICAR_INT(3, c.numero_valores);
    VERIFICAR_TOKEN(TOKEN_NUMERO, c.valores[0].tipo);
    VERIFICAR_INT(10, c.valores[0].numero);
    VERIFICAR_TOKEN(TOKEN_STRING, c.valores[1].tipo);
    VERIFICAR_STR("Ana", c.valores[1].str);
    VERIFICAR_TOKEN(TOKEN_NUMERO, c.valores[2].tipo);
    VERIFICAR_INT(20, c.valores[2].numero);
}

static void teste_insert_um_unico_valor(void)
{
    Parser p; Comando c;
    if (!ESPERAR_OK("INSERT INTO t VALUES (5);")) return;
    VERIFICAR_INT(1, c.numero_valores);
}

static void teste_insert_palavras_reservadas_sem_diferenciar_caixa(void)
{
    Parser p; Comando c;
    if (!ESPERAR_OK("insert into T values (1);")) return;
    VERIFICAR_STR("T", c.tabela);
    VERIFICAR_INT(1, c.numero_valores);
}

static void teste_insert_string_vazia(void)
{
    Parser p; Comando c;
    if (!ESPERAR_OK("INSERT INTO t VALUES ('');")) return;
    VERIFICAR_TOKEN(TOKEN_STRING, c.valores[0].tipo);
    VERIFICAR_STR("", c.valores[0].str);
}

static void teste_insert_string_com_simbolos_e_espacos(void)
{
    Parser p; Comando c;
    if (!ESPERAR_OK("INSERT INTO t VALUES ('a b,c;', 1);")) return;
    VERIFICAR_INT(2, c.numero_valores);
    VERIFICAR_STR("a b,c;", c.valores[0].str);   /* ';' e ',' dentro da string */
    VERIFICAR_INT(1, c.valores[1].numero);
}

static void teste_insert_string_no_limite_de_tamanho(void)
{
    Parser p; Comando c;
    if (ESPERAR_OK("INSERT INTO t VALUES ('12345678');"))      /* 8: ok */
        VERIFICAR_STR("12345678", c.valores[0].str);

    ESPERAR_ERRO("INSERT INTO t VALUES ('123456789');",        /* 9: erro */
                 "token invalido");
}

static void teste_insert_string_sem_fechar(void)
{
    ESPERAR_ERRO("INSERT INTO t VALUES ('abc);", "token invalido");
}

static void teste_insert_numeros_limites(void)
{
    Parser p; Comando c;
    if (ESPERAR_OK("INSERT INTO t VALUES (0, 2147483647);")) {
        VERIFICAR_INT(0, c.valores[0].numero);
        VERIFICAR_INT(INT_MAX, c.valores[1].numero);
    }
    ESPERAR_ERRO("INSERT INTO t VALUES (2147483648);", "fora do intervalo de INT");
    ESPERAR_ERRO("INSERT INTO t VALUES (99999999999999999999);",
                 "numero '99999999999999999999' fora do intervalo de INT");
}

static void teste_insert_limite_de_valores(void)
{
    Parser p; Comando c;
    char buf[1024];

    gerar_insert(buf, sizeof buf, MAX_VALORES);              /* 16: ok */
    if (ESPERAR_OK(buf)) {
        VERIFICAR_INT(MAX_VALORES, c.numero_valores);
        VERIFICAR_INT(15, c.valores[MAX_VALORES - 1].numero);
    }

    gerar_insert(buf, sizeof buf, MAX_VALORES + 1);          /* 17: erro */
    VERIFICAR_INT(0, analisar(&p, &c, buf));
    VERIFICAR_CONTEM(p.mensagem, "numero maximo de valores (16) excedido");
}

static void teste_insert_erros_de_sintaxe(void)
{
    ESPERAR_ERRO("INSERT",                       "fim inesperado do texto (esperado: INTO)");
    ESPERAR_ERRO("INSERT t VALUES (1);",         "encontrado 't' (esperado: INTO)");
    ESPERAR_ERRO("INSERT INTO VALUES (1);",      "encontrado 'VALUES' (esperado: nome da tabela)");
    ESPERAR_ERRO("INSERT INTO t (1);",           "encontrado '(' (esperado: VALUES)");
    ESPERAR_ERRO("INSERT INTO t VALUES 1);",     "encontrado '1' (esperado: '(')");
    ESPERAR_ERRO("INSERT INTO t VALUES ();",     "encontrado ')' (esperado: numero ou string)");
    ESPERAR_ERRO("INSERT INTO t VALUES (1,);",   "encontrado ')' (esperado: numero ou string)");
    ESPERAR_ERRO("INSERT INTO t VALUES (,1);",   "encontrado ',' (esperado: numero ou string)");
    ESPERAR_ERRO("INSERT INTO t VALUES (abc);",  "encontrado 'abc' (esperado: numero ou string)");
    ESPERAR_ERRO("INSERT INTO t VALUES (1;",     "encontrado ';' (esperado: ')' ou ',')");
    ESPERAR_ERRO("INSERT INTO t VALUES (1 2);",  "encontrado '2' (esperado: ')' ou ',')");
    ESPERAR_ERRO("INSERT INTO t VALUES (1)",     "fim inesperado do texto (esperado: ';')");
    ESPERAR_ERRO("INSERT INTO t VALUES (1) x;",  "encontrado 'x' (esperado: ';')");
}

static void teste_insert_valores_nao_suportados(void)
{
    ESPERAR_ERRO("INSERT INTO t VALUES (-1);",   "token invalido '-'");
    ESPERAR_ERRO("INSERT INTO t VALUES (1.5);",  "token invalido '.'");
    ESPERAR_ERRO("INSERT INTO t VALUES (12abc);","token invalido '12abc'");
    ESPERAR_ERRO("INSERT INTO t VALUES (NULL);", "esperado: numero ou string");
    ESPERAR_ERRO("INSERT INTO t VALUES (1 OR 2);","construcao nao suportada 'OR'");
}

/* ================================================================== */
/* 8. SELECT                                                           */
/* ================================================================== */

static void teste_select_sem_where(void)
{
    Parser p; Comando c;
    if (!ESPERAR_OK("SELECT * FROM t;")) return;

    VERIFICAR_INT(CMD_SELECT, c.tipo);
    VERIFICAR_STR("t", c.tabela);
    VERIFICAR_INT(0, c.numero_condicoes);
    VERIFICAR_INT(0, c.numero_colunas);
    VERIFICAR_INT(0, c.numero_valores);
    VERIFICAR_TOKEN(TOKEN_FIM, p.atual.tipo);
}

static void teste_select_com_uma_condicao(void)
{
    Parser p; Comando c;
    if (!ESPERAR_OK("SELECT * FROM t WHERE id = 10;")) return;

    VERIFICAR_INT(1, c.numero_condicoes);
    VERIFICAR_STR("id", c.condicoes[0].coluna);
    VERIFICAR_TOKEN(TOKEN_IGUAL, c.condicoes[0].operacao);
    VERIFICAR_TOKEN(TOKEN_NUMERO, c.condicoes[0].valor.tipo);
    VERIFICAR_INT(10, c.condicoes[0].valor.numero);
}

static void teste_select_com_varias_condicoes_and(void)
{
    Parser p; Comando c;
    if (!ESPERAR_OK("SELECT * FROM t WHERE a = 1 AND b > 'x' AND c <= 30;")) return;

    VERIFICAR_INT(3, c.numero_condicoes);

    VERIFICAR_STR("a", c.condicoes[0].coluna);
    VERIFICAR_TOKEN(TOKEN_IGUAL, c.condicoes[0].operacao);
    VERIFICAR_INT(1, c.condicoes[0].valor.numero);

    VERIFICAR_STR("b", c.condicoes[1].coluna);
    VERIFICAR_TOKEN(TOKEN_MAIOR, c.condicoes[1].operacao);
    VERIFICAR_TOKEN(TOKEN_STRING, c.condicoes[1].valor.tipo);
    VERIFICAR_STR("x", c.condicoes[1].valor.str);

    VERIFICAR_STR("c", c.condicoes[2].coluna);
    VERIFICAR_TOKEN(TOKEN_MENOR_IGUAL, c.condicoes[2].operacao);
    VERIFICAR_INT(30, c.condicoes[2].valor.numero);
}

static void teste_select_todos_os_operadores(void)
{
    static const struct { const char *texto; TipoToken op; } casos[] = {
        { "SELECT * FROM t WHERE a = 1;",  TOKEN_IGUAL       },
        { "SELECT * FROM t WHERE a <> 1;", TOKEN_DIFERENTE   },
        { "SELECT * FROM t WHERE a != 1;", TOKEN_DIFERENTE   },
        { "SELECT * FROM t WHERE a < 1;",  TOKEN_MENOR       },
        { "SELECT * FROM t WHERE a <= 1;", TOKEN_MENOR_IGUAL },
        { "SELECT * FROM t WHERE a > 1;",  TOKEN_MAIOR       },
        { "SELECT * FROM t WHERE a >= 1;", TOKEN_MAIOR_IGUAL },
        { "SELECT * FROM t WHERE a=1;",    TOKEN_IGUAL       },
        { "SELECT * FROM t WHERE a<>1;",   TOKEN_DIFERENTE   },
        { "SELECT * FROM t WHERE a>=1;",   TOKEN_MAIOR_IGUAL },
    };

    for (size_t i = 0; i < sizeof casos / sizeof casos[0]; i++) {
        Parser p; Comando c;
        if (esperar_ok_(__LINE__, &p, &c, casos[i].texto)) {
            VERIFICAR_INT(1, c.numero_condicoes);
            if (c.numero_condicoes == 1)
                VERIFICAR_TOKEN(casos[i].op, c.condicoes[0].operacao);
        }
    }
}

static void teste_select_condicao_com_string(void)
{
    Parser p; Comando c;
    if (!ESPERAR_OK("SELECT * FROM t WHERE nome <> 'Ana';")) return;
    VERIFICAR_TOKEN(TOKEN_STRING, c.condicoes[0].valor.tipo);
    VERIFICAR_STR("Ana", c.condicoes[0].valor.str);
}

static void teste_select_palavras_reservadas_sem_diferenciar_caixa(void)
{
    Parser p; Comando c;
    if (!ESPERAR_OK("select * from T where A = 1 and B = 2;")) return;
    VERIFICAR_STR("T", c.tabela);
    VERIFICAR_INT(2, c.numero_condicoes);
    VERIFICAR_STR("A", c.condicoes[0].coluna);
    VERIFICAR_STR("B", c.condicoes[1].coluna);
}

static void teste_select_sem_espacos_e_multilinha(void)
{
    Parser p; Comando c;
    if (ESPERAR_OK("SELECT*FROM t;"))
        VERIFICAR_STR("t", c.tabela);

    if (ESPERAR_OK("SELECT *\nFROM t\nWHERE a = 1\n  AND b = 2\n;"))
        VERIFICAR_INT(2, c.numero_condicoes);
}

static void teste_select_limite_de_condicoes(void)
{
    Parser p; Comando c;
    char buf[1024];

    gerar_select(buf, sizeof buf, MAX_CONDICOES);            /* 16: ok */
    if (ESPERAR_OK(buf))
        VERIFICAR_INT(MAX_CONDICOES, c.numero_condicoes);

    gerar_select(buf, sizeof buf, MAX_CONDICOES + 1);        /* 17: erro */
    VERIFICAR_INT(0, analisar(&p, &c, buf));
    VERIFICAR_CONTEM(p.mensagem, "numero maximo de condicoes (16) excedido");
}

static void teste_select_erros_de_sintaxe(void)
{
    ESPERAR_ERRO("SELECT",                         "fim inesperado do texto (esperado: '*')");
    ESPERAR_ERRO("SELECT a, b FROM t;",            "encontrado 'a' (esperado: '*')");
    ESPERAR_ERRO("SELECT * t;",                    "encontrado 't' (esperado: FROM)");
    ESPERAR_ERRO("SELECT * FROM;",                 "encontrado ';' (esperado: nome da tabela)");
    ESPERAR_ERRO("SELECT * FROM t",                "fim inesperado do texto (esperado: ';', WHERE ou AND)");
    ESPERAR_ERRO("SELECT * FROM t x;",             "encontrado 'x' (esperado: ';', WHERE ou AND)");
    ESPERAR_ERRO("SELECT * FROM t WHERE;",         "encontrado ';' (esperado: nome da coluna)");
    ESPERAR_ERRO("SELECT * FROM t WHERE a;",       "encontrado ';' (esperado: operador");
    ESPERAR_ERRO("SELECT * FROM t WHERE a 5;",     "encontrado '5' (esperado: operador");
    ESPERAR_ERRO("SELECT * FROM t WHERE a =;",     "encontrado ';' (esperado: numero ou string)");
    ESPERAR_ERRO("SELECT * FROM t WHERE a = b;",   "encontrado 'b' (esperado: numero ou string)");
    ESPERAR_ERRO("SELECT * FROM t WHERE a = 1 AND;", "encontrado ';' (esperado: nome da coluna)");
    ESPERAR_ERRO("SELECT * FROM t WHERE a = 1 WHERE b = 2;", "encontrado 'WHERE'");
    ESPERAR_ERRO("SELECT * FROM t AND a = 1;",    "encontrado 'AND'");
    ESPERAR_ERRO("SELECT * FROM t WHERE a = 1",  "fim inesperado do texto");
}

static void teste_select_operadores_malformados(void)
{
    ESPERAR_ERRO("SELECT * FROM t WHERE a ! 1;",  "token invalido '!'");
    ESPERAR_ERRO("SELECT * FROM t WHERE a == 1;", "encontrado '=' (esperado: numero ou string)");
    ESPERAR_ERRO("SELECT * FROM t WHERE a => 1;", "encontrado '>' (esperado: numero ou string)");
    ESPERAR_ERRO("SELECT * FROM t WHERE a =< 1;", "encontrado '<' (esperado: numero ou string)");
    ESPERAR_ERRO("SELECT * FROM t WHERE a >< 1;", "encontrado '<' (esperado: numero ou string)");
}

static void teste_select_construcoes_nao_suportadas(void)
{
    ESPERAR_ERRO("SELECT * FROM t WHERE a = 1 OR b = 2;", "construcao nao suportada 'OR'");
    ESPERAR_ERRO("SELECT * FROM a JOIN b ON a.x = b.x;",  "construcao nao suportada 'JOIN'");
    ESPERAR_ERRO("SELECT * FROM a INNER JOIN b;",         "construcao nao suportada 'INNER'");
    ESPERAR_ERRO("SELECT * FROM t ORDER BY a;",           "construcao nao suportada 'ORDER'");
    ESPERAR_ERRO("SELECT * FROM t GROUP BY a;",           "construcao nao suportada 'GROUP'");
    ESPERAR_ERRO("SELECT COUNT(*) FROM t;",               "construcao nao suportada 'COUNT'");
    ESPERAR_ERRO("SELECT SUM(a) FROM t;",                 "construcao nao suportada 'SUM'");
    ESPERAR_ERRO("SELECT * FROM t WHERE a = 1 ORDER BY a;", "construcao nao suportada 'ORDER'");
}

/* ================================================================== */
/* 9. parse_comando (despacho, estado, posicoes de erro)               */
/* ================================================================== */

static void teste_parse_comando_inicio_invalido(void)
{
    ESPERAR_ERRO("DROP TABLE t;",   "encontrado 'DROP' (esperado: CREATE, INSERT ou SELECT)");
    ESPERAR_ERRO("UPDATE t;",       "encontrado 'UPDATE' (esperado: CREATE, INSERT ou SELECT)");
    ESPERAR_ERRO("TABLE t;",        "encontrado 'TABLE'");
    ESPERAR_ERRO("123;",            "encontrado '123'");
    ESPERAR_ERRO("'abc';",          "encontrado 'abc'");
    ESPERAR_ERRO(";",               "encontrado ';'");
    ESPERAR_ERRO("(",               "encontrado '('");
    ESPERAR_ERRO("",                "fim inesperado do texto (esperado: CREATE, INSERT ou SELECT)");
    ESPERAR_ERRO("   \n  ",         "fim inesperado do texto");
    ESPERAR_ERRO("OR",              "construcao nao suportada 'OR'");
    ESPERAR_ERRO("@",               "token invalido '@'");
}

static void teste_parse_comando_zera_a_estrutura(void)
{
    Parser p; Comando c;

    memset(&c, 0xFF, sizeof c);               /* lixo */
    if (!ESPERAR_OK("SELECT * FROM t;")) return;
    VERIFICAR_INT(0, c.numero_colunas);
    VERIFICAR_INT(0, c.numero_valores);
    VERIFICAR_INT(0, c.numero_condicoes);

    memset(&c, 0xFF, sizeof c);
    if (!ESPERAR_OK("CREATE TABLE t (a INT);")) return;
    VERIFICAR_INT(1, c.numero_colunas);       /* nao acumula do lixo */
    VERIFICAR_INT(0, c.numero_valores);
    VERIFICAR_INT(0, c.numero_condicoes);
}

static void teste_parse_comando_reuso_da_estrutura_nao_acumula(void)
{
    Parser p; Comando c;

    iniciar_parser(&p, "CREATE TABLE a (x INT, y INT); CREATE TABLE b (z CHAR);");
    VERIFICAR_INT(1, parse_comando(&p, &c));
    VERIFICAR_INT(2, c.numero_colunas);
    VERIFICAR_INT(1, parse_comando(&p, &c));
    VERIFICAR_STR("b", c.tabela);
    VERIFICAR_INT(1, c.numero_colunas);
    VERIFICAR_STR("z", c.colunas[0].nome);
}

static void teste_parse_comando_para_logo_apos_o_ponto_virgula(void)
{
    Parser p; Comando c;
    if (!ESPERAR_OK("SELECT * FROM t; lixo")) return;
    VERIFICAR_TOKEN(TOKEN_IDENTACAO, p.atual.tipo);
    VERIFICAR_STR("lixo", p.atual.lexema);
}

static void teste_posicao_do_erro_linha_unica(void)
{
    ESPERAR_ERRO("CREATE TABLE t (a FOO);", "Linha 1, coluna 19:");
    ESPERAR_ERRO("SELECT * FROM t",         "Linha 1, coluna 16: fim inesperado");
    ESPERAR_ERRO("INSERT INTO t VALUES (1 2);", "Linha 1, coluna 25:");
}

static void teste_posicao_do_erro_multilinha(void)
{
    ESPERAR_ERRO("CREATE TABLE t (\n  a FOO);", "Linha 2, coluna 5:");
    ESPERAR_ERRO("SELECT *\nFROM t\nWHERE a ?;",  "Linha 3, coluna 9: token invalido '?'");
}

/* ================================================================== */
/* 10. Programa com varios comandos e recuperacao de erros             */
/* ================================================================== */

static void teste_programa_vazio(void)
{
    int ok, erros;
    executar_programa("", &ok, &erros, NULL, 0);
    VERIFICAR_INT(0, ok);
    VERIFICAR_INT(0, erros);

    executar_programa("  \n\t  ", &ok, &erros, NULL, 0);
    VERIFICAR_INT(0, ok);
    VERIFICAR_INT(0, erros);
}

static void teste_programa_tres_comandos_validos(void)
{
    int ok, erros;
    executar_programa("CREATE TABLE t (a INT, b CHAR);\n"
                      "INSERT INTO t VALUES (1, 'x');\n"
                      "SELECT * FROM t WHERE a = 1;\n",
                      &ok, &erros, NULL, 0);
    VERIFICAR_INT(3, ok);
    VERIFICAR_INT(0, erros);
}

static void teste_programa_recupera_apos_erro_e_continua(void)
{
    int ok, erros;
    char msg[MAX_MESSAGEM];

    executar_programa("CREATE TABLE t (a INT);\n"
                      "INSERT INTO t VALUES (1,;\n"      /* erro */
                      "SELECT * FROM t;\n",
                      &ok, &erros, msg, sizeof msg);
    VERIFICAR_INT(2, ok);
    VERIFICAR_INT(1, erros);
    VERIFICAR_CONTEM(msg, "Linha 2,");
    VERIFICAR_CONTEM(msg, "esperado: numero ou string");
}

static void teste_programa_varios_erros_consecutivos(void)
{
    int ok, erros;
    executar_programa("DROP TABLE t; ALTER x; SELECT * FROM t;", &ok, &erros, NULL, 0);
    VERIFICAR_INT(1, ok);
    VERIFICAR_INT(2, erros);
}

static void teste_programa_erro_sem_ponto_virgula_final_termina(void)
{
    int ok, erros;
    executar_programa("SELECT * FROM", &ok, &erros, NULL, 0);
    VERIFICAR_INT(0, ok);
    VERIFICAR_INT(1, erros);
}

static void teste_programa_erro_descarta_ate_o_proximo_ponto_virgula(void)
{
    int ok, erros;
    /* falta ';' no CREATE: o erro e no SELECT, que e descartado ate o ';' */
    executar_programa("CREATE TABLE t (a INT) SELECT * FROM t; SELECT * FROM t;",
                      &ok, &erros, NULL, 0);
    VERIFICAR_INT(1, ok);
    VERIFICAR_INT(1, erros);
}

static void teste_programa_ponto_virgula_solto_conta_como_erro(void)
{
    int ok, erros;
    executar_programa(";", &ok, &erros, NULL, 0);
    VERIFICAR_INT(0, ok);
    VERIFICAR_INT(1, erros);
}

static void teste_programa_erro_em_segunda_linha_reporta_linha_correta(void)
{
    int ok, erros;
    char msg[MAX_MESSAGEM];
    executar_programa("SELECT * FROM t;\nSELECT FROM t;", &ok, &erros, msg, sizeof msg);
    VERIFICAR_INT(1, ok);
    VERIFICAR_INT(1, erros);
    VERIFICAR_STR("Linha 2, coluna 8: encontrado 'FROM' (esperado: '*')", msg);
}

static void teste_programa_erros_lexicos_nao_travam(void)
{
    int ok, erros;
    executar_programa("@@@ ### ; 'aberta", &ok, &erros, NULL, 0);
    VERIFICAR_INT(0, ok);
    VERIFICAR_INT(2, erros);
}

/* ================================================================== */
/* 11. Impressao da AST (captura de stdout)                            */
/* ================================================================== */

static void teste_imprimir_valor(void)
{
    Parser p;
    Valor v;
    char saida[64];

    iniciar_parser(&p, "5");
    parse_valor(&p, &v);
    captura_iniciar();
    imprimir_valor(&v);
    captura_finalizar(saida, sizeof saida);
    VERIFICAR_STR("5", saida);

    iniciar_parser(&p, "'ab'");
    parse_valor(&p, &v);
    captura_iniciar();
    imprimir_valor(&v);
    captura_finalizar(saida, sizeof saida);
    VERIFICAR_STR("'ab'", saida);
}

static void teste_imprimir_create(void)
{
    Parser p; Comando c;
    char saida[512];
    if (!ESPERAR_OK("CREATE TABLE t (id INT, nome CHAR);")) return;

    captura_iniciar();
    imprimir_comando(&c);
    captura_finalizar(saida, sizeof saida);
    VERIFICAR_STR("CREATE TABLE\n"
                  "  tabela : t\n"
                  "  colunas:\n"
                  "    - id : INT\n"
                  "    - nome : CHAR\n", saida);
}

static void teste_imprimir_insert(void)
{
    Parser p; Comando c;
    char saida[512];
    if (!ESPERAR_OK("INSERT INTO t VALUES (1, 'abc');")) return;

    captura_iniciar();
    imprimir_comando(&c);
    captura_finalizar(saida, sizeof saida);
    VERIFICAR_STR("INSERT\n"
                  "  tabela : t\n"
                  "  valores: 1, 'abc'\n", saida);
}

static void teste_imprimir_select_sem_where(void)
{
    Parser p; Comando c;
    char saida[512];
    if (!ESPERAR_OK("SELECT * FROM t;")) return;

    captura_iniciar();
    imprimir_comando(&c);
    captura_finalizar(saida, sizeof saida);
    VERIFICAR_STR("SELECT\n"
                  "  tabela : t\n"
                  "  where  : (nenhum)\n", saida);
}

static void teste_imprimir_select_com_where(void)
{
    Parser p; Comando c;
    char saida[512];
    if (!ESPERAR_OK("SELECT * FROM t WHERE a = 1 AND b <> 'x';")) return;

    captura_iniciar();
    imprimir_comando(&c);
    captura_finalizar(saida, sizeof saida);
    VERIFICAR_STR("SELECT\n"
                  "  tabela : t\n"
                  "  where  :\n"
                  "        a = 1\n"
                  "    AND b <> 'x'\n", saida);
}

/* ================================================================== */
/* main                                                                */
/* ================================================================== */

int main(void)
{
    printf("=== Testes unitarios do Parser ===\n\n");

    /* 1. Inicializacao */
    EXECUTAR(teste_iniciar_parser_texto_vazio);
    EXECUTAR(teste_iniciar_parser_le_primeiro_token);
    EXECUTAR(teste_iniciar_parser_ignora_espacos_iniciais);
    EXECUTAR(teste_avancar_parser_nunca_para_em_espaco);
    EXECUTAR(teste_iniciar_parser_limpa_mensagem_anterior);

    /* 2. Tratamento de erros */
    EXECUTAR(teste_erro_formato_prefixo_e_retorno);
    EXECUTAR(teste_erro_formato_trunca_sem_estourar_buffer);
    EXECUTAR(teste_falha_token_erro);
    EXECUTAR(teste_falha_token_nao_suportado);
    EXECUTAR(teste_falha_fim_inesperado);
    EXECUTAR(teste_falha_token_comum);
    EXECUTAR(teste_consumir_token_sucesso_avanca_e_devolve_token);
    EXECUTAR(teste_consumir_token_saida_nula);
    EXECUTAR(teste_consumir_token_falha_nao_avanca);
    EXECUTAR(teste_sincronizar_consome_ate_depois_do_ponto_virgula);
    EXECUTAR(teste_sincronizar_ja_em_ponto_virgula);
    EXECUTAR(teste_sincronizar_sem_ponto_virgula_vai_ate_o_fim);
    EXECUTAR(teste_sincronizar_em_fim_nao_trava);
    EXECUTAR(teste_sincronizar_so_pula_um_ponto_virgula);
    EXECUTAR(teste_sincronizar_pula_tokens_invalidos);

    /* 3. Operadores */
    EXECUTAR(teste_e_operador_verdadeiros);
    EXECUTAR(teste_e_operador_falsos);
    EXECUTAR(teste_simbolo_operacao);

    /* 4. parse_valor */
    EXECUTAR(teste_parse_valor_numero);
    EXECUTAR(teste_parse_valor_string);
    EXECUTAR(teste_parse_valor_numero_zero_e_zeros_a_esquerda);
    EXECUTAR(teste_parse_valor_limites_de_int);
    EXECUTAR(teste_parse_valor_invalido_nao_avanca);
    EXECUTAR(teste_parse_valor_rejeita_tokens_nao_valor);

    /* 5. parse_condicao */
    EXECUTAR(teste_parse_condicao_numero);
    EXECUTAR(teste_parse_condicao_string);
    EXECUTAR(teste_parse_condicao_erros);

    /* 6. CREATE */
    EXECUTAR(teste_create_uma_coluna);
    EXECUTAR(teste_create_varias_colunas);
    EXECUTAR(teste_create_palavras_reservadas_sem_diferenciar_caixa);
    EXECUTAR(teste_create_sem_espacos_ao_redor_de_simbolos);
    EXECUTAR(teste_create_com_quebras_de_linha_e_tabs);
    EXECUTAR(teste_create_nome_com_underscore_e_digitos);
    EXECUTAR(teste_create_limite_de_colunas);
    EXECUTAR(teste_create_coluna_duplicada);
    EXECUTAR(teste_create_mesmo_nome_de_tabela_e_coluna_e_permitido);
    EXECUTAR(teste_create_nome_no_limite_do_lexema);
    EXECUTAR(teste_create_erros_de_sintaxe);
    EXECUTAR(teste_create_tokens_invalidos_e_nao_suportados);

    /* 7. INSERT */
    EXECUTAR(teste_insert_numeros);
    EXECUTAR(teste_insert_strings);
    EXECUTAR(teste_insert_valores_mistos);
    EXECUTAR(teste_insert_um_unico_valor);
    EXECUTAR(teste_insert_palavras_reservadas_sem_diferenciar_caixa);
    EXECUTAR(teste_insert_string_vazia);
    EXECUTAR(teste_insert_string_com_simbolos_e_espacos);
    EXECUTAR(teste_insert_string_no_limite_de_tamanho);
    EXECUTAR(teste_insert_string_sem_fechar);
    EXECUTAR(teste_insert_numeros_limites);
    EXECUTAR(teste_insert_limite_de_valores);
    EXECUTAR(teste_insert_erros_de_sintaxe);
    EXECUTAR(teste_insert_valores_nao_suportados);

    /* 8. SELECT */
    EXECUTAR(teste_select_sem_where);
    EXECUTAR(teste_select_com_uma_condicao);
    EXECUTAR(teste_select_com_varias_condicoes_and);
    EXECUTAR(teste_select_todos_os_operadores);
    EXECUTAR(teste_select_condicao_com_string);
    EXECUTAR(teste_select_palavras_reservadas_sem_diferenciar_caixa);
    EXECUTAR(teste_select_sem_espacos_e_multilinha);
    EXECUTAR(teste_select_limite_de_condicoes);
    EXECUTAR(teste_select_erros_de_sintaxe);
    EXECUTAR(teste_select_operadores_malformados);
    EXECUTAR(teste_select_construcoes_nao_suportadas);

    /* 9. parse_comando */
    EXECUTAR(teste_parse_comando_inicio_invalido);
    EXECUTAR(teste_parse_comando_zera_a_estrutura);
    EXECUTAR(teste_parse_comando_reuso_da_estrutura_nao_acumula);
    EXECUTAR(teste_parse_comando_para_logo_apos_o_ponto_virgula);
    EXECUTAR(teste_posicao_do_erro_linha_unica);
    EXECUTAR(teste_posicao_do_erro_multilinha);

    /* 10. Programa completo */
    EXECUTAR(teste_programa_vazio);
    EXECUTAR(teste_programa_tres_comandos_validos);
    EXECUTAR(teste_programa_recupera_apos_erro_e_continua);
    EXECUTAR(teste_programa_varios_erros_consecutivos);
    EXECUTAR(teste_programa_erro_sem_ponto_virgula_final_termina);
    EXECUTAR(teste_programa_erro_descarta_ate_o_proximo_ponto_virgula);
    EXECUTAR(teste_programa_ponto_virgula_solto_conta_como_erro);
    EXECUTAR(teste_programa_erro_em_segunda_linha_reporta_linha_correta);
    EXECUTAR(teste_programa_erros_lexicos_nao_travam);

    /* 11. Impressao */
    EXECUTAR(teste_imprimir_valor);
    EXECUTAR(teste_imprimir_create);
    EXECUTAR(teste_imprimir_insert);
    EXECUTAR(teste_imprimir_select_sem_where);
    EXECUTAR(teste_imprimir_select_com_where);

    printf("\n=== Resultado: %d/%d testes passaram (%d verificacoes, %d falhas) ===\n",
           g_testes_ok, g_testes, g_verificacoes, g_falhas);
    return g_falhas ? 1 : 0;
}
