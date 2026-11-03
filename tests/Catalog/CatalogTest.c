/*
 * test_catalog.c - testes das funcoes de Catalog.c
 *
 * Cobre SOMENTE as funcoes definidas em Catalog.c:
 *
 *   utilitarios ........ erro_cat, tamanho_do_tipo, nome_valido,
 *                        montar_nome_arquivo
 *   consulta ........... catalogo_iniciar, catalogo_buscar_tabela,
 *                        tabela_buscar_coluna
 *   CREATE TABLE ....... catalogo_adicionar_tabela
 *   semantica .......... valor_compativel, catalogo_validar_insert,
 *                        catalogo_validar_select
 *   armazenamento ...... selecionar_arquivo_tabela, empacotar_char,
 *                        desempacotar_char, catalogo_executar_insert
 *   SELECT ............. comparar_int, comparar_char, registro_corresponde,
 *                        imprimir_registro, callback_select,
 *                        catalogo_executar_select
 *   execucao ........... catalogo_executar
 *   impressao .......... imprimir_tabela, catalogo_imprimir
 *   persistencia ....... catalogo_salvar, ler_palavra, ler_inteiro,
 *                        carregar_de_arquivo, catalogo_carregar
 *
 * As funcoes de Pager.c, Cache.c, B_Plus_Tree.c e Parser.c NAO sao testadas
 * aqui; elas so aparecem como dependencia (ex.: contexto_igual, nome_token e
 * a arvore B+ usada nos testes de INSERT/SELECT).
 *
 * Como compilar (na pasta onde estao Catalog.c, Pager.c, Cache.c, ...):
 *
 *   gcc -Wall -Wextra -o test_catalog test_catalog.c
 *   ./test_catalog
 *
 * O programa devolve 0 se tudo passou e 1 se algum teste falhou.
 * Os testes rodam dentro de um diretorio temporario (/tmp/test_catalog_XXXXXX),
 * entao nenhum .dat ou .cat e criado na pasta do projeto.
 *
 * Requer POSIX (dup2, mkdtemp). Como Catalog.c e incluido diretamente, as
 * funcoes static ficam acessiveis para teste.
 */

#include "Catalog.c"   /* tem que vir primeiro: define _POSIX_C_SOURCE */

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Mini framework de testes                                            */
/* ------------------------------------------------------------------ */

static int g_verificacoes = 0;
static int g_falhas       = 0;

#define CHECK(cond) do {                                                     \
        g_verificacoes++;                                                    \
        if (!(cond)) {                                                       \
            g_falhas++;                                                      \
            printf("    FALHA %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                    \
    } while (0)

#define CHECK_INT(obtido, esperado) do {                                     \
        long long o_ = (long long)(obtido), e_ = (long long)(esperado);      \
        g_verificacoes++;                                                    \
        if (o_ != e_) {                                                      \
            g_falhas++;                                                      \
            printf("    FALHA %s:%d: %s\n      obtido: %lld, esperado: %lld\n", \
                   __FILE__, __LINE__, #obtido, o_, e_);                     \
        }                                                                    \
    } while (0)

#define CHECK_STR(obtido, esperado) do {                                     \
        const char *o_ = (obtido), *e_ = (esperado);                         \
        g_verificacoes++;                                                    \
        if (!o_ || strcmp(o_, e_) != 0) {                                    \
            g_falhas++;                                                      \
            printf("    FALHA %s:%d: %s\n      obtido:   \"%s\"\n      esperado: \"%s\"\n", \
                   __FILE__, __LINE__, #obtido, o_ ? o_ : "(null)", e_);     \
        }                                                                    \
    } while (0)

#define CHECK_CONTEM(texto, trecho) do {                                     \
        const char *t_ = (texto), *p_ = (trecho);                            \
        g_verificacoes++;                                                    \
        if (!t_ || !strstr(t_, p_)) {                                        \
            g_falhas++;                                                      \
            printf("    FALHA %s:%d: esperava encontrar \"%s\" em:\n      \"%s\"\n", \
                   __FILE__, __LINE__, p_, t_ ? t_ : "(null)");              \
        }                                                                    \
    } while (0)

#define CHECK_NAO_CONTEM(texto, trecho) do {                                 \
        const char *t_ = (texto), *p_ = (trecho);                            \
        g_verificacoes++;                                                    \
        if (t_ && strstr(t_, p_)) {                                          \
            g_falhas++;                                                      \
            printf("    FALHA %s:%d: NAO esperava encontrar \"%s\" em:\n      \"%s\"\n", \
                   __FILE__, __LINE__, p_, t_);                              \
        }                                                                    \
    } while (0)

static void executar_teste(const char *nome, void (*fn)(void))
{
    int antes = g_falhas;
    printf("[ RUN  ] %s\n", nome);
    fn();
    printf("%s %s\n", g_falhas == antes ? "[  OK  ]" : "[FALHOU]", nome);
}

#define RODAR(fn) executar_teste(#fn, fn)

/* ------------------------------------------------------------------ */
/* Estado global dos testes                                            */
/* Estruturas grandes ficam fora da pilha.                             */
/* ------------------------------------------------------------------ */

static Catalogo g_cat;
static Catalogo g_cat2;
static Comando  g_cmd;
static char     g_saida[32768];

/* tipo do campo 'tipo' de Comando, sem depender do nome do enum */
typedef __typeof__(((Comando *)0)->tipo) TipoCmd;

/* ------------------------------------------------------------------ */
/* Construtores de dados                                               */
/* ------------------------------------------------------------------ */

static Coluna coluna(const char *nome, TipoToken tipo)
{
    Coluna c;
    memset(&c, 0, sizeof c);
    snprintf(c.nome, sizeof c.nome, "%s", nome);
    c.tipo = tipo;
    return c;
}

static Valor num(int n)
{
    Valor v;
    memset(&v, 0, sizeof v);
    v.tipo   = TOKEN_NUMERO;
    v.numero = n;
    return v;
}

static Valor txt(const char *s)
{
    Valor v;
    memset(&v, 0, sizeof v);
    v.tipo = TOKEN_STRING;
    snprintf(v.str, sizeof v.str, "%s", s);
    return v;
}

static void cmd_iniciar(TipoCmd tipo, const char *tabela)
{
    memset(&g_cmd, 0, sizeof g_cmd);
    g_cmd.tipo = tipo;
    snprintf(g_cmd.tabela, sizeof g_cmd.tabela, "%s", tabela);
}

static void cmd_coluna(const char *nome, TipoToken tipo)
{
    g_cmd.colunas[g_cmd.numero_colunas++] = coluna(nome, tipo);
}

static void cmd_valor(Valor v)
{
    g_cmd.valores[g_cmd.numero_valores++] = v;
}

static void cmd_condicao(const char *col, TipoToken op, Valor v)
{
    Condicao *c = &g_cmd.condicoes[g_cmd.numero_condicoes++];
    memset(c, 0, sizeof *c);
    snprintf(c->coluna, sizeof c->coluna, "%s", col);
    c->operacao = op;
    c->valor    = v;
}

static Registro registro(int id, int valor)
{
    Registro r;
    memset(&r, 0, sizeof r);
    r.id    = id;
    r.valor = valor;
    return r;
}

/* Registro cuja segunda coluna e CHAR (ja empacotada em 4 bytes) */
static Registro registro_char(int id, const char *texto)
{
    int32_t empacotado = 0;
    empacotar_char(texto, &empacotado);
    return registro(id, empacotado);
}

/* Preenche 'saida' com 'n' copias de 'c' */
static void repetir(char *saida, char c, int n)
{
    memset(saida, c, (size_t)n);
    saida[n] = '\0';
}

/* Catalogo com tres tabelas:
 *   alunos(id INT, nome CHAR)   numeros(id INT, valor INT)   solo(id INT) */
static void catalogo_padrao(void)
{
    Coluna alunos[]  = { coluna("id", TOKEN_INT), coluna("nome",  TOKEN_CHAR) };
    Coluna numeros[] = { coluna("id", TOKEN_INT), coluna("valor", TOKEN_INT)  };
    Coluna solo[]    = { coluna("id", TOKEN_INT) };

    catalogo_iniciar(&g_cat);
    CHECK_INT(catalogo_adicionar_tabela(&g_cat, "alunos",  alunos,  2), 1);
    CHECK_INT(catalogo_adicionar_tabela(&g_cat, "numeros", numeros, 2), 1);
    CHECK_INT(catalogo_adicionar_tabela(&g_cat, "solo",    solo,    1), 1);
}

/* ------------------------------------------------------------------ */
/* Arquivos e captura de stdout                                        */
/* ------------------------------------------------------------------ */

static int ler_arquivo(const char *caminho, char *buf, size_t capacidade)
{
    FILE *f = fopen(caminho, "rb");
    size_t n;

    buf[0] = '\0';
    if (!f)
        return -1;
    n = fread(buf, 1, capacidade - 1, f);
    buf[n] = '\0';
    fclose(f);
    return (int)n;
}

static void escrever_arquivo(const char *caminho, const char *conteudo)
{
    FILE *f = fopen(caminho, "wb");
    if (f) {
        fputs(conteudo, f);
        fclose(f);
    }
}

/* Arquivo temporario ja aberto para leitura com o conteudo dado */
static FILE *abrir_texto(const char *conteudo)
{
    FILE *f = tmpfile();
    if (f) {
        fputs(conteudo, f);
        rewind(f);
    }
    return f;
}

static int existe_arquivo(const char *caminho)
{
    return access(caminho, F_OK) == 0;
}

static int g_stdout_original = -1;

static void captura_iniciar(void)
{
    int fd;
    fflush(stdout);
    g_stdout_original = dup(STDOUT_FILENO);
    fd = open("captura.txt", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    dup2(fd, STDOUT_FILENO);
    close(fd);
}

/* Restaura stdout e devolve tudo o que foi impresso desde captura_iniciar.
 * Nenhum CHECK pode rodar entre captura_iniciar e captura_finalizar. */
static const char *captura_finalizar(void)
{
    fflush(stdout);
    dup2(g_stdout_original, STDOUT_FILENO);
    close(g_stdout_original);
    ler_arquivo("captura.txt", g_saida, sizeof g_saida);
    return g_saida;
}

/* ================================================================== */
/* Utilitarios                                                         */
/* ================================================================== */

static void test_erro_cat(void)
{
    char longa[MAX_MESSAGEM * 2 + 1];
    int r;

    catalogo_iniciar(&g_cat);

    r = erro_cat(&g_cat, "tabela '%s' tem %d colunas", "x", 3);
    CHECK_INT(r, 0);                                   /* sempre devolve 0 */
    CHECK_STR(g_cat.mensagem, "tabela 'x' tem 3 colunas");

    /* a mensagem nova substitui a anterior */
    erro_cat(&g_cat, "outra");
    CHECK_STR(g_cat.mensagem, "outra");

    /* mensagem maior que o buffer e truncada e continua terminada em '\0' */
    repetir(longa, 'x', MAX_MESSAGEM * 2);
    erro_cat(&g_cat, "%s", longa);
    CHECK_INT(strlen(g_cat.mensagem), MAX_MESSAGEM - 1);
}

static void test_tamanho_do_tipo(void)
{
    CHECK_INT(tamanho_do_tipo(TOKEN_INT),  TAM_INT);
    CHECK_INT(tamanho_do_tipo(TOKEN_CHAR), TAM_CHAR);
    /* qualquer tipo diferente de INT cai em CHAR (a validacao de tipos
     * e feita em catalogo_adicionar_tabela) */
    CHECK_INT(tamanho_do_tipo(TOKEN_NUMERO), TAM_CHAR);
}

static void test_nome_valido(void)
{
    CHECK(nome_valido("a"));
    CHECK(nome_valido("alunos"));
    CHECK(nome_valido("Alunos"));
    CHECK(nome_valido("_tmp"));
    CHECK(nome_valido("_"));
    CHECK(nome_valido("a1"));
    CHECK(nome_valido("nota_final_2"));
    CHECK(nome_valido("ABC_123"));

    CHECK(!nome_valido(""));
    CHECK(!nome_valido("1abc"));        /* nao pode comecar com digito  */
    CHECK(!nome_valido("9"));
    CHECK(!nome_valido("a-b"));
    CHECK(!nome_valido("a b"));
    CHECK(!nome_valido("a.b"));
    CHECK(!nome_valido("a;"));
    CHECK(!nome_valido("-a"));
    CHECK(!nome_valido(" a"));
    CHECK(!nome_valido("a\n"));
}

static void test_montar_nome_arquivo(void)
{
    char saida[TAM_ARQUIVO];
    char longo[MAX_LEXEMA * 3];
    size_t n;

    montar_nome_arquivo(saida, "alunos");
    CHECK_STR(saida, "alunos.dat");

    montar_nome_arquivo(saida, "Alunos");          /* vai para minusculas */
    CHECK_STR(saida, "alunos.dat");

    montar_nome_arquivo(saida, "NOTAS_2");
    CHECK_STR(saida, "notas_2.dat");

    montar_nome_arquivo(saida, "a");
    CHECK_STR(saida, "a.dat");

    /* nome gigante e cortado em MAX_LEXEMA-1 caracteres + ".dat" */
    repetir(longo, 'X', MAX_LEXEMA * 3 - 1);
    montar_nome_arquivo(saida, longo);
    n = strlen(saida);
    CHECK_INT(n, (MAX_LEXEMA - 1) + 4);
    CHECK(n < TAM_ARQUIVO);
    CHECK_STR(saida + n - 4, ".dat");
    CHECK(saida[0] == 'x' && saida[MAX_LEXEMA - 2] == 'x');
}

/* ================================================================== */
/* Consulta ao catalogo                                                */
/* ================================================================== */

static void test_catalogo_iniciar(void)
{
    memset(&g_cat, 0xFF, sizeof g_cat);           /* lixo na memoria */
    catalogo_iniciar(&g_cat);

    CHECK_INT(g_cat.numero_tabelas, 0);
    CHECK_STR(g_cat.mensagem, "");
    CHECK_STR(g_cat.tabelas[0].nome, "");
    CHECK_INT(g_cat.tabelas[0].numero_colunas, 0);
    CHECK_INT(g_cat.tabelas[MAX_TABELAS - 1].numero_colunas, 0);
}

static void test_catalogo_buscar_tabela(void)
{
    catalogo_iniciar(&g_cat);
    CHECK(catalogo_buscar_tabela(&g_cat, "alunos") == NULL);   /* vazio */

    catalogo_padrao();

    CHECK(catalogo_buscar_tabela(&g_cat, "alunos")  == &g_cat.tabelas[0]);
    CHECK(catalogo_buscar_tabela(&g_cat, "numeros") == &g_cat.tabelas[1]);
    CHECK(catalogo_buscar_tabela(&g_cat, "solo")    == &g_cat.tabelas[2]);

    /* nomes nao diferenciam maiusculas de minusculas */
    CHECK(catalogo_buscar_tabela(&g_cat, "ALUNOS")  == &g_cat.tabelas[0]);
    CHECK(catalogo_buscar_tabela(&g_cat, "NuMeRoS") == &g_cat.tabelas[1]);

    CHECK(catalogo_buscar_tabela(&g_cat, "inexistente") == NULL);
    CHECK(catalogo_buscar_tabela(&g_cat, "") == NULL);
    CHECK(catalogo_buscar_tabela(&g_cat, "aluno") == NULL);    /* prefixo */
    CHECK(catalogo_buscar_tabela(&g_cat, "alunos2") == NULL);

    /* so olha as tabelas registradas (numero_tabelas) */
    g_cat.numero_tabelas = 1;
    CHECK(catalogo_buscar_tabela(&g_cat, "numeros") == NULL);
}

static void test_tabela_buscar_coluna(void)
{
    const TabelaCat *alunos;
    const TabelaCat *solo;

    catalogo_padrao();
    alunos = catalogo_buscar_tabela(&g_cat, "alunos");
    solo   = catalogo_buscar_tabela(&g_cat, "solo");

    CHECK_INT(tabela_buscar_coluna(alunos, "id"),   0);
    CHECK_INT(tabela_buscar_coluna(alunos, "nome"), 1);
    CHECK_INT(tabela_buscar_coluna(alunos, "ID"),   0);   /* sem case */
    CHECK_INT(tabela_buscar_coluna(alunos, "NoMe"), 1);
    CHECK_INT(tabela_buscar_coluna(alunos, "idade"), -1);
    CHECK_INT(tabela_buscar_coluna(alunos, ""),      -1);
    CHECK_INT(tabela_buscar_coluna(alunos, "nom"),   -1);

    CHECK_INT(tabela_buscar_coluna(solo, "id"),   0);
    CHECK_INT(tabela_buscar_coluna(solo, "nome"), -1);
}

/* ================================================================== */
/* catalogo_adicionar_tabela                                           */
/* ================================================================== */

static void esperar_erro_adicionar(int linha, const char *nome,
                                   const Coluna *cols, int n, const char *trecho)
{
    int falhas_antes = g_falhas;
    int tabelas_antes = g_cat.numero_tabelas;
    int r = catalogo_adicionar_tabela(&g_cat, nome, cols, n);

    CHECK_INT(r, 0);
    CHECK_INT(g_cat.numero_tabelas, tabelas_antes);   /* nada foi registrado */
    CHECK_CONTEM(g_cat.mensagem, trecho);
    if (g_falhas != falhas_antes)
        printf("      (chamada em test_catalog.c:%d, tabela '%s')\n", linha, nome);
}

#define ESPERAR_ERRO(nome, cols, n, trecho) \
    esperar_erro_adicionar(__LINE__, nome, cols, n, trecho)

static void test_adicionar_tabela_sucesso(void)
{
    Coluna alunos[]  = { coluna("id", TOKEN_INT), coluna("nome",  TOKEN_CHAR) };
    Coluna numeros[] = { coluna("id", TOKEN_INT), coluna("valor", TOKEN_INT)  };
    Coluna solo[]    = { coluna("chave", TOKEN_INT) };
    const TabelaCat *t;

    catalogo_iniciar(&g_cat);

    CHECK_INT(catalogo_adicionar_tabela(&g_cat, "Alunos", alunos, 2), 1);
    CHECK_INT(g_cat.numero_tabelas, 1);

    t = &g_cat.tabelas[0];
    CHECK_STR(t->nome, "Alunos");                 /* guarda a grafia original */
    CHECK_STR(t->arquivo, "alunos.dat");          /* arquivo em minusculas    */
    CHECK_INT(t->numero_colunas, 2);
    CHECK_STR(t->colunas[0].nome, "id");
    CHECK(t->colunas[0].tipo == TOKEN_INT);
    CHECK_INT(t->colunas[0].tamanho, TAM_INT);
    CHECK_INT(t->colunas[0].deslocamento, 0);
    CHECK_STR(t->colunas[1].nome, "nome");
    CHECK(t->colunas[1].tipo == TOKEN_CHAR);
    CHECK_INT(t->colunas[1].tamanho, TAM_CHAR);
    CHECK_INT(t->colunas[1].deslocamento, TAM_INT);
    CHECK_INT(t->tamanho_util, TAM_INT + TAM_CHAR);
    CHECK_INT(t->tamanho_registro, TAM_REGISTRO);

    /* tabela com duas colunas INT */
    CHECK_INT(catalogo_adicionar_tabela(&g_cat, "numeros", numeros, 2), 1);
    t = &g_cat.tabelas[1];
    CHECK_INT(t->colunas[1].tamanho, TAM_INT);
    CHECK_INT(t->colunas[1].deslocamento, TAM_INT);
    CHECK_INT(t->tamanho_util, 2 * TAM_INT);

    /* tabela com uma unica coluna */
    CHECK_INT(catalogo_adicionar_tabela(&g_cat, "solo", solo, 1), 1);
    t = &g_cat.tabelas[2];
    CHECK_INT(t->numero_colunas, 1);
    CHECK_INT(t->tamanho_util, TAM_INT);
    CHECK_INT(t->tamanho_registro, TAM_REGISTRO);
    CHECK_STR(t->arquivo, "solo.dat");

    CHECK_INT(g_cat.numero_tabelas, 3);

    /* nome comecando com '_' e com digitos e valido */
    {
        Coluna c[] = { coluna("_id1", TOKEN_INT) };
        CHECK_INT(catalogo_adicionar_tabela(&g_cat, "_t1", c, 1), 1);
    }
}

static void test_adicionar_tabela_nome_e_limites(void)
{
    Coluna ok[] = { coluna("id", TOKEN_INT), coluna("nome", TOKEN_CHAR) };
    Coluna um[] = { coluna("id", TOKEN_INT) };
    char nome[16];

    catalogo_iniciar(&g_cat);

    /* nome de tabela invalido */
    ESPERAR_ERRO("",      ok, 2, "nome de tabela invalido");
    ESPERAR_ERRO("1abc",  ok, 2, "nome de tabela invalido");
    ESPERAR_ERRO("a-b",   ok, 2, "nome de tabela invalido");
    ESPERAR_ERRO("a b",   ok, 2, "nome de tabela invalido");
    CHECK_CONTEM(g_cat.mensagem, "a b");           /* a mensagem cita o nome */

    /* quantidade de colunas fora de 1..MAX_COLUNAS */
    ESPERAR_ERRO("t", ok, 0,               "deve ter de 1 a");
    ESPERAR_ERRO("t", ok, -1,              "deve ter de 1 a");
    ESPERAR_ERRO("t", ok, MAX_COLUNAS + 1, "deve ter de 1 a");

    /* duplicada (sem diferenciar maiusculas) */
    CHECK_INT(catalogo_adicionar_tabela(&g_cat, "alunos", ok, 2), 1);
    ESPERAR_ERRO("alunos", ok, 2, "ja existe");
    ESPERAR_ERRO("ALUNOS", ok, 2, "ja existe");
    CHECK_CONTEM(g_cat.mensagem, "ALUNOS");

    /* limite de tabelas: ja temos 1, completa ate MAX_TABELAS */
    for (int i = 1; i < MAX_TABELAS; i++) {
        snprintf(nome, sizeof nome, "t%d", i);
        CHECK_INT(catalogo_adicionar_tabela(&g_cat, nome, um, 1), 1);
    }
    CHECK_INT(g_cat.numero_tabelas, MAX_TABELAS);
    ESPERAR_ERRO("excedente", um, 1, "numero maximo de tabelas");
    /* com o catalogo cheio, repetir um nome continua sendo erro de duplicata */
    ESPERAR_ERRO("alunos", um, 1, "ja existe");
}

static void test_adicionar_tabela_colunas_invalidas(void)
{
    catalogo_iniciar(&g_cat);

    {
        Coluna c[] = { coluna("id", TOKEN_INT), coluna("1nome", TOKEN_CHAR) };
        ESPERAR_ERRO("t", c, 2, "nome de coluna invalido");
        CHECK_CONTEM(g_cat.mensagem, "1nome");
    }
    {
        Coluna c[] = { coluna("", TOKEN_INT) };
        ESPERAR_ERRO("t", c, 1, "nome de coluna invalido");
    }
    {
        Coluna c[] = { coluna("id", TOKEN_INT), coluna("x-y", TOKEN_CHAR) };
        ESPERAR_ERRO("t", c, 2, "nome de coluna invalido");
    }
    {
        Coluna c[] = { coluna("id", TOKEN_INT), coluna("nome", TOKEN_NUMERO) };
        ESPERAR_ERRO("t", c, 2, "tipo desconhecido");
        CHECK_CONTEM(g_cat.mensagem, "nome");
    }
    {
        Coluna c[] = { coluna("id", TOKEN_STRING) };
        ESPERAR_ERRO("t", c, 1, "tipo desconhecido");
    }
    {
        /* duplicada, sem diferenciar maiusculas */
        Coluna c[] = { coluna("id", TOKEN_INT), coluna("ID", TOKEN_CHAR) };
        ESPERAR_ERRO("t", c, 2, "duplicada");
    }
    {
        Coluna c[] = { coluna("id", TOKEN_INT), coluna("id", TOKEN_INT) };
        ESPERAR_ERRO("t", c, 2, "duplicada");
    }
}

static void test_adicionar_tabela_restricoes_fisicas(void)
{
    Coluna muitas[MAX_COLUNAS];
    int n;

    catalogo_iniciar(&g_cat);

    /* mais bytes do que cabe em um registro */
    n = TAM_REGISTRO / TAM_INT + 1;
    if (n <= MAX_COLUNAS) {
        char nome[16];
        for (int i = 0; i < n; i++) {
            snprintf(nome, sizeof nome, "c%d", i);
            muitas[i] = coluna(nome, TOKEN_INT);
        }
        ESPERAR_ERRO("grande", muitas, n, "ocupam");
        CHECK_CONTEM(g_cat.mensagem, "tamanho fixo");
    } else {
        printf("    (aviso: MAX_COLUNAS=%d impede testar o estouro de TAM_REGISTRO)\n",
               MAX_COLUNAS);
    }

    /* integracao com a arvore B+: primeira coluna INT, no maximo 2 colunas */
    {
        Coluna so_char[] = { coluna("nome", TOKEN_CHAR) };
        ESPERAR_ERRO("t1", so_char, 1, "arvore B+");
    }
    {
        Coluna char_primeiro[] = { coluna("nome", TOKEN_CHAR), coluna("id", TOKEN_INT) };
        ESPERAR_ERRO("t2", char_primeiro, 2, "arvore B+");
    }
    if (MAX_COLUNAS >= 3) {
        Coluna tres[] = { coluna("a", TOKEN_INT), coluna("b", TOKEN_INT),
                          coluna("c", TOKEN_INT) };
        /* 3 colunas: ou estoura o registro, ou esbarra na regra da arvore B+ */
        ESPERAR_ERRO("t3", tres, 3,
                     (3 * TAM_INT <= TAM_REGISTRO) ? "arvore B+" : "ocupam");
    }

    /* nenhuma das tentativas acima deixou rastro no catalogo */
    CHECK_INT(g_cat.numero_tabelas, 0);
    CHECK(catalogo_buscar_tabela(&g_cat, "t1") == NULL);
}

/* ================================================================== */
/* Analise semantica                                                   */
/* ================================================================== */

static void test_valor_compativel(void)
{
    const TabelaCat *t;
    const ColunaCat *id, *nome;
    char s_limite[TAM_CHAR + 1], s_excede[TAM_CHAR + 2];

    catalogo_padrao();
    t    = catalogo_buscar_tabela(&g_cat, "alunos");
    id   = &t->colunas[0];
    nome = &t->colunas[1];

    /* INT aceita numero */
    {
        Valor v = num(42);
        CHECK_INT(valor_compativel(&g_cat, t, id, &v, "INSERT"), 1);
        v = num(-7);
        CHECK_INT(valor_compativel(&g_cat, t, id, &v, "INSERT"), 1);
        v = num(0);
        CHECK_INT(valor_compativel(&g_cat, t, id, &v, "INSERT"), 1);
    }

    /* INT rejeita string */
    {
        Valor v = txt("abc");
        CHECK_INT(valor_compativel(&g_cat, t, id, &v, "INSERT"), 0);
        CHECK_CONTEM(g_cat.mensagem, "INSERT");
        CHECK_CONTEM(g_cat.mensagem, "'id'");
        CHECK_CONTEM(g_cat.mensagem, "'alunos'");
        CHECK_CONTEM(g_cat.mensagem, "e INT");
        CHECK_CONTEM(g_cat.mensagem, "'abc'");
    }

    /* CHAR aceita string de ate TAM_CHAR caracteres (inclusive vazia) */
    repetir(s_limite, 'x', TAM_CHAR);
    repetir(s_excede, 'x', TAM_CHAR + 1);
    {
        Valor v = txt("");
        CHECK_INT(valor_compativel(&g_cat, t, nome, &v, "INSERT"), 1);
        v = txt("a");
        CHECK_INT(valor_compativel(&g_cat, t, nome, &v, "INSERT"), 1);
        v = txt(s_limite);
        CHECK_INT(valor_compativel(&g_cat, t, nome, &v, "INSERT"), 1);
    }

    /* CHAR rejeita string maior que a coluna */
    {
        Valor v = txt(s_excede);
        CHECK_INT(valor_compativel(&g_cat, t, nome, &v, "INSERT"), 0);
        CHECK_CONTEM(g_cat.mensagem, s_excede);
        CHECK_CONTEM(g_cat.mensagem, "comporta");
        CHECK_CONTEM(g_cat.mensagem, "'nome'");
    }

    /* CHAR rejeita numero */
    {
        Valor v = num(42);
        CHECK_INT(valor_compativel(&g_cat, t, nome, &v, "INSERT"), 0);
        CHECK_CONTEM(g_cat.mensagem, "e CHAR");
        CHECK_CONTEM(g_cat.mensagem, "42");
        CHECK_CONTEM(g_cat.mensagem, "numero");
    }

    /* o contexto aparece como prefixo da mensagem */
    {
        Valor v = txt("abc");
        CHECK_INT(valor_compativel(&g_cat, t, id, &v, "WHERE"), 0);
        CHECK_CONTEM(g_cat.mensagem, "WHERE:");
    }
}

static void test_validar_insert(void)
{
    catalogo_padrao();

    /* validos */
    cmd_iniciar(CMD_INSERT, "alunos");
    cmd_valor(num(1)); cmd_valor(txt("Ana"));
    CHECK_INT(catalogo_validar_insert(&g_cat, &g_cmd), 1);

    cmd_iniciar(CMD_INSERT, "ALUNOS");                     /* sem case */
    cmd_valor(num(1)); cmd_valor(txt("Ana"));
    CHECK_INT(catalogo_validar_insert(&g_cat, &g_cmd), 1);

    cmd_iniciar(CMD_INSERT, "numeros");
    cmd_valor(num(1)); cmd_valor(num(-50));
    CHECK_INT(catalogo_validar_insert(&g_cat, &g_cmd), 1);

    cmd_iniciar(CMD_INSERT, "solo");
    cmd_valor(num(9));
    CHECK_INT(catalogo_validar_insert(&g_cat, &g_cmd), 1);

    /* tabela inexistente */
    cmd_iniciar(CMD_INSERT, "fantasma");
    cmd_valor(num(1));
    CHECK_INT(catalogo_validar_insert(&g_cat, &g_cmd), 0);
    CHECK_CONTEM(g_cat.mensagem, "tabela 'fantasma' nao existe");

    /* quantidade de valores diferente do numero de colunas */
    cmd_iniciar(CMD_INSERT, "alunos");
    cmd_valor(num(1));
    CHECK_INT(catalogo_validar_insert(&g_cat, &g_cmd), 0);
    CHECK_CONTEM(g_cat.mensagem, "espera 2 valor(es)");
    CHECK_CONTEM(g_cat.mensagem, "recebeu 1");

    cmd_iniciar(CMD_INSERT, "alunos");
    cmd_valor(num(1)); cmd_valor(txt("Ana")); cmd_valor(num(3));
    CHECK_INT(catalogo_validar_insert(&g_cat, &g_cmd), 0);
    CHECK_CONTEM(g_cat.mensagem, "recebeu 3");

    cmd_iniciar(CMD_INSERT, "alunos");                     /* nenhum valor */
    CHECK_INT(catalogo_validar_insert(&g_cat, &g_cmd), 0);
    CHECK_CONTEM(g_cat.mensagem, "recebeu 0");

    /* tipo errado na 1a coluna */
    cmd_iniciar(CMD_INSERT, "alunos");
    cmd_valor(txt("x")); cmd_valor(txt("Ana"));
    CHECK_INT(catalogo_validar_insert(&g_cat, &g_cmd), 0);
    CHECK_CONTEM(g_cat.mensagem, "'id'");
    CHECK_CONTEM(g_cat.mensagem, "INSERT");

    /* tipo errado na 2a coluna */
    cmd_iniciar(CMD_INSERT, "alunos");
    cmd_valor(num(1)); cmd_valor(num(2));
    CHECK_INT(catalogo_validar_insert(&g_cat, &g_cmd), 0);
    CHECK_CONTEM(g_cat.mensagem, "'nome'");

    cmd_iniciar(CMD_INSERT, "numeros");
    cmd_valor(num(1)); cmd_valor(txt("x"));
    CHECK_INT(catalogo_validar_insert(&g_cat, &g_cmd), 0);
    CHECK_CONTEM(g_cat.mensagem, "'valor'");

    /* string grande demais */
    cmd_iniciar(CMD_INSERT, "alunos");
    cmd_valor(num(1)); cmd_valor(txt("abcdefgh"));
    CHECK_INT(catalogo_validar_insert(&g_cat, &g_cmd), 0);
    CHECK_CONTEM(g_cat.mensagem, "comporta");
}

static void test_validar_select(void)
{
    catalogo_padrao();

    /* sem WHERE */
    cmd_iniciar(CMD_SELECT, "alunos");
    CHECK_INT(catalogo_validar_select(&g_cat, &g_cmd), 1);

    /* tabela inexistente */
    cmd_iniciar(CMD_SELECT, "fantasma");
    CHECK_INT(catalogo_validar_select(&g_cat, &g_cmd), 0);
    CHECK_CONTEM(g_cat.mensagem, "tabela 'fantasma' nao existe");

    /* condicoes validas (inclusive combinadas e sem diferenciar maiusculas) */
    cmd_iniciar(CMD_SELECT, "alunos");
    cmd_condicao("id", TOKEN_MAIOR, num(1));
    CHECK_INT(catalogo_validar_select(&g_cat, &g_cmd), 1);

    cmd_iniciar(CMD_SELECT, "ALUNOS");
    cmd_condicao("ID", TOKEN_MAIOR_IGUAL, num(1));
    cmd_condicao("NOME", TOKEN_DIFERENTE, txt("Ana"));
    CHECK_INT(catalogo_validar_select(&g_cat, &g_cmd), 1);

    /* coluna inexistente */
    cmd_iniciar(CMD_SELECT, "alunos");
    cmd_condicao("idade", TOKEN_IGUAL, num(1));
    CHECK_INT(catalogo_validar_select(&g_cat, &g_cmd), 0);
    CHECK_CONTEM(g_cat.mensagem, "coluna 'idade' nao existe na tabela 'alunos'");

    /* so a 2a condicao e invalida */
    cmd_iniciar(CMD_SELECT, "alunos");
    cmd_condicao("id", TOKEN_IGUAL, num(1));
    cmd_condicao("idade", TOKEN_IGUAL, num(1));
    CHECK_INT(catalogo_validar_select(&g_cat, &g_cmd), 0);
    CHECK_CONTEM(g_cat.mensagem, "'idade'");

    /* tipo incompativel: o contexto da mensagem e WHERE */
    cmd_iniciar(CMD_SELECT, "alunos");
    cmd_condicao("id", TOKEN_IGUAL, txt("abc"));
    CHECK_INT(catalogo_validar_select(&g_cat, &g_cmd), 0);
    CHECK_CONTEM(g_cat.mensagem, "WHERE");
    CHECK_CONTEM(g_cat.mensagem, "e INT");

    cmd_iniciar(CMD_SELECT, "alunos");
    cmd_condicao("nome", TOKEN_IGUAL, num(5));
    CHECK_INT(catalogo_validar_select(&g_cat, &g_cmd), 0);
    CHECK_CONTEM(g_cat.mensagem, "e CHAR");

    /* string maior que a coluna CHAR */
    cmd_iniciar(CMD_SELECT, "alunos");
    cmd_condicao("nome", TOKEN_IGUAL, txt("abcdefgh"));
    CHECK_INT(catalogo_validar_select(&g_cat, &g_cmd), 0);
    CHECK_CONTEM(g_cat.mensagem, "comporta");
}

/* ================================================================== */
/* Empacotamento de CHAR                                               */
/* ================================================================== */

static void test_empacotar_char(void)
{
    int32_t v = 123;

    /* little-endian: 1o caractere no byte menos significativo */
    CHECK_INT(empacotar_char("A", &v), 1);
    CHECK_INT(v, 0x41);

    CHECK_INT(empacotar_char("AB", &v), 1);
    CHECK_INT(v, 0x4241);

    CHECK_INT(empacotar_char("abcd", &v), 1);
    CHECK_INT(v, 0x64636261);

    CHECK_INT(empacotar_char("", &v), 1);
    CHECK_INT(v, 0);

    /* bytes altos: o bit de sinal vem do 4o caractere */
    CHECK_INT(empacotar_char("\xFF\xFF\xFF\xFF", &v), 1);
    CHECK_INT(v, -1);

    CHECK_INT(empacotar_char("\x01\x00", &v), 1);
    CHECK_INT(v, 1);

    /* mais de 4 caracteres: recusa e nao mexe na saida */
    v = 123;
    CHECK_INT(empacotar_char("abcde", &v), 0);
    CHECK_INT(v, 123);
    CHECK_INT(empacotar_char("abcdefghij", &v), 0);
    CHECK_INT(v, 123);
}

static void test_desempacotar_char(void)
{
    char s[5];

    desempacotar_char(0, s);
    CHECK_STR(s, "");

    desempacotar_char(0x41, s);
    CHECK_STR(s, "A");

    desempacotar_char(0x4241, s);
    CHECK_STR(s, "AB");

    desempacotar_char(0x64636261, s);
    CHECK_STR(s, "abcd");
    CHECK_INT(s[4], 0);                     /* sempre terminada em '\0' */

    /* um '\0' no meio encerra a string */
    desempacotar_char(0x00620061, s);
    CHECK_STR(s, "a");

    /* 4 bytes 0xFF (valor -1) */
    desempacotar_char(-1, s);
    CHECK_INT(strlen(s), 4);
    CHECK_INT((unsigned char)s[0], 0xFF);
    CHECK_INT((unsigned char)s[3], 0xFF);
    CHECK_INT(s[4], 0);
}

static void test_empacotar_desempacotar_ida_e_volta(void)
{
    const char *textos[] = { "", "a", "ab", "abc", "abcd", "Ana", "Z9_", "  x " };
    char s[5];

    for (size_t i = 0; i < sizeof textos / sizeof textos[0]; i++) {
        int32_t v = -1;
        CHECK_INT(empacotar_char(textos[i], &v), 1);
        desempacotar_char(v, s);
        CHECK_STR(s, textos[i]);
    }
}

/* ================================================================== */
/* Comparacoes e filtro de registros                                   */
/* ================================================================== */

/* Resultado esperado para (a<b), (a==b), (a>b) em cada operador */
static const struct {
    TipoToken op;
    int menor, igual, maior;
} OPERADORES[] = {
    { TOKEN_IGUAL,        0, 1, 0 },
    { TOKEN_DIFERENTE,    1, 0, 1 },
    { TOKEN_MENOR,        1, 0, 0 },
    { TOKEN_MENOR_IGUAL,  1, 1, 0 },
    { TOKEN_MAIOR,        0, 0, 1 },
    { TOKEN_MAIOR_IGUAL,  0, 1, 1 },
};
#define NUM_OPERADORES ((int)(sizeof OPERADORES / sizeof OPERADORES[0]))

static void test_comparar_int(void)
{
    for (int i = 0; i < NUM_OPERADORES; i++) {
        TipoToken op = OPERADORES[i].op;
        CHECK_INT(comparar_int(1, op, 2), OPERADORES[i].menor);
        CHECK_INT(comparar_int(2, op, 2), OPERADORES[i].igual);
        CHECK_INT(comparar_int(3, op, 2), OPERADORES[i].maior);
    }

    /* negativos e extremos */
    CHECK_INT(comparar_int(-5, TOKEN_MENOR, 3), 1);
    CHECK_INT(comparar_int(-5, TOKEN_MAIOR, -6), 1);
    CHECK_INT(comparar_int(INT32_MIN, TOKEN_MENOR, INT32_MAX), 1);
    CHECK_INT(comparar_int(INT32_MAX, TOKEN_MAIOR_IGUAL, INT32_MAX), 1);
    CHECK_INT(comparar_int(INT32_MIN, TOKEN_IGUAL, INT32_MIN), 1);

    /* operador que nao e de comparacao nunca casa */
    CHECK_INT(comparar_int(1, TOKEN_INT, 1), 0);
    CHECK_INT(comparar_int(1, TOKEN_NUMERO, 1), 0);
}

static void test_comparar_char(void)
{
    for (int i = 0; i < NUM_OPERADORES; i++) {
        TipoToken op = OPERADORES[i].op;
        CHECK_INT(comparar_char("a", op, "b"), OPERADORES[i].menor);
        CHECK_INT(comparar_char("b", op, "b"), OPERADORES[i].igual);
        CHECK_INT(comparar_char("c", op, "b"), OPERADORES[i].maior);
    }

    CHECK_INT(comparar_char("", TOKEN_IGUAL, ""), 1);
    CHECK_INT(comparar_char("", TOKEN_MENOR, "a"), 1);
    CHECK_INT(comparar_char("a", TOKEN_MAIOR, ""), 1);
    CHECK_INT(comparar_char("ab", TOKEN_MAIOR, "a"), 1);      /* prefixo */
    CHECK_INT(comparar_char("abc", TOKEN_MENOR, "abd"), 1);

    /* CHAR e sensivel a maiusculas (diferente de nomes de tabela/coluna) */
    CHECK_INT(comparar_char("Ana", TOKEN_IGUAL, "ana"), 0);
    CHECK_INT(comparar_char("A", TOKEN_MENOR, "a"), 1);

    CHECK_INT(comparar_char("a", TOKEN_INT, "a"), 0);
    CHECK_INT(comparar_char("a", TOKEN_NUMERO, "a"), 0);
}

static void test_registro_corresponde(void)
{
    const TabelaCat *alunos, *numeros;
    ContextoSelect ctx;

    catalogo_padrao();
    alunos  = catalogo_buscar_tabela(&g_cat, "alunos");
    numeros = catalogo_buscar_tabela(&g_cat, "numeros");

    /* sem condicoes: todo registro corresponde */
    cmd_iniciar(CMD_SELECT, "alunos");
    ctx = (ContextoSelect){ alunos, &g_cmd, 0 };
    {
        Registro r = registro_char(1, "Ana");
        CHECK_INT(registro_corresponde(&ctx, &r), 1);
    }

    /* condicao sobre a 1a coluna (id) */
    cmd_iniciar(CMD_SELECT, "alunos");
    cmd_condicao("id", TOKEN_IGUAL, num(5));
    ctx = (ContextoSelect){ alunos, &g_cmd, 0 };
    {
        Registro sim = registro_char(5, "Ana"), nao = registro_char(6, "Ana");
        CHECK_INT(registro_corresponde(&ctx, &sim), 1);
        CHECK_INT(registro_corresponde(&ctx, &nao), 0);
    }

    /* nome da coluna sem diferenciar maiusculas */
    cmd_iniciar(CMD_SELECT, "alunos");
    cmd_condicao("ID", TOKEN_MAIOR, num(5));
    ctx = (ContextoSelect){ alunos, &g_cmd, 0 };
    {
        Registro sim = registro_char(6, "Ana"), nao = registro_char(5, "Ana");
        CHECK_INT(registro_corresponde(&ctx, &sim), 1);
        CHECK_INT(registro_corresponde(&ctx, &nao), 0);
    }

    /* condicao sobre a 2a coluna INT (valor) */
    cmd_iniciar(CMD_SELECT, "numeros");
    cmd_condicao("valor", TOKEN_MAIOR, num(10));
    ctx = (ContextoSelect){ numeros, &g_cmd, 0 };
    {
        Registro sim = registro(1, 11), nao = registro(1, 10), neg = registro(1, -50);
        CHECK_INT(registro_corresponde(&ctx, &sim), 1);
        CHECK_INT(registro_corresponde(&ctx, &nao), 0);
        CHECK_INT(registro_corresponde(&ctx, &neg), 0);
    }

    /* condicao sobre a 2a coluna CHAR (desempacotada antes de comparar) */
    cmd_iniciar(CMD_SELECT, "alunos");
    cmd_condicao("nome", TOKEN_IGUAL, txt("Bia"));
    ctx = (ContextoSelect){ alunos, &g_cmd, 0 };
    {
        Registro sim = registro_char(1, "Bia"), nao = registro_char(1, "Ana");
        Registro prefixo = registro_char(1, "Bi");
        CHECK_INT(registro_corresponde(&ctx, &sim), 1);
        CHECK_INT(registro_corresponde(&ctx, &nao), 0);
        CHECK_INT(registro_corresponde(&ctx, &prefixo), 0);
    }

    cmd_iniciar(CMD_SELECT, "alunos");
    cmd_condicao("nome", TOKEN_MAIOR, txt("B"));
    ctx = (ContextoSelect){ alunos, &g_cmd, 0 };
    {
        Registro sim = registro_char(1, "Caio"), nao = registro_char(1, "Ana");
        CHECK_INT(registro_corresponde(&ctx, &sim), 1);
        CHECK_INT(registro_corresponde(&ctx, &nao), 0);
    }

    /* string vazia */
    cmd_iniciar(CMD_SELECT, "alunos");
    cmd_condicao("nome", TOKEN_IGUAL, txt(""));
    ctx = (ContextoSelect){ alunos, &g_cmd, 0 };
    {
        Registro vazio = registro_char(1, ""), cheio = registro_char(1, "A");
        CHECK_INT(registro_corresponde(&ctx, &vazio), 1);
        CHECK_INT(registro_corresponde(&ctx, &cheio), 0);
    }

    /* varias condicoes: todas precisam valer (AND) */
    cmd_iniciar(CMD_SELECT, "alunos");
    cmd_condicao("id", TOKEN_MAIOR_IGUAL, num(2));
    cmd_condicao("nome", TOKEN_DIFERENTE, txt("Ana"));
    ctx = (ContextoSelect){ alunos, &g_cmd, 0 };
    {
        Registro ok = registro_char(2, "Bia");
        Registro id_baixo = registro_char(1, "Bia");
        Registro nome_igual = registro_char(3, "Ana");
        CHECK_INT(registro_corresponde(&ctx, &ok), 1);
        CHECK_INT(registro_corresponde(&ctx, &id_baixo), 0);
        CHECK_INT(registro_corresponde(&ctx, &nome_igual), 0);
    }

    /* comportamento atual: condicao sobre coluna inexistente e ignorada
     * (catalogo_validar_select ja barra esse caso antes da execucao) */
    cmd_iniciar(CMD_SELECT, "alunos");
    cmd_condicao("idade", TOKEN_IGUAL, num(99));
    ctx = (ContextoSelect){ alunos, &g_cmd, 0 };
    {
        Registro r = registro_char(1, "Ana");
        CHECK_INT(registro_corresponde(&ctx, &r), 1);
    }
}

/* ================================================================== */
/* Impressao                                                           */
/* ================================================================== */

static void test_imprimir_registro(void)
{
    const TabelaCat *alunos, *numeros, *solo;
    const char *saida;
    Registro r;

    catalogo_padrao();
    alunos  = catalogo_buscar_tabela(&g_cat, "alunos");
    numeros = catalogo_buscar_tabela(&g_cat, "numeros");
    solo    = catalogo_buscar_tabela(&g_cat, "solo");

    r = registro_char(7, "Ana");
    captura_iniciar();
    imprimir_registro(alunos, &r);
    saida = captura_finalizar();
    CHECK_STR(saida, "  7 | 'Ana'\n");

    r = registro_char(8, "");
    captura_iniciar();
    imprimir_registro(alunos, &r);
    saida = captura_finalizar();
    CHECK_STR(saida, "  8 | ''\n");

    r = registro_char(9, "abcd");
    captura_iniciar();
    imprimir_registro(alunos, &r);
    saida = captura_finalizar();
    CHECK_STR(saida, "  9 | 'abcd'\n");

    r = registro(7, 42);
    captura_iniciar();
    imprimir_registro(numeros, &r);
    saida = captura_finalizar();
    CHECK_STR(saida, "  7 | 42\n");

    r = registro(7, -42);
    captura_iniciar();
    imprimir_registro(numeros, &r);
    saida = captura_finalizar();
    CHECK_STR(saida, "  7 | -42\n");

    /* tabela de uma coluna so imprime o id */
    r = registro(7, 0);
    captura_iniciar();
    imprimir_registro(solo, &r);
    saida = captura_finalizar();
    CHECK_STR(saida, "  7\n");
}

static void test_callback_select(void)
{
    const TabelaCat *alunos;
    ContextoSelect ctx;
    const char *saida;
    Registro sim, nao;
    int r;

    catalogo_padrao();
    alunos = catalogo_buscar_tabela(&g_cat, "alunos");

    cmd_iniciar(CMD_SELECT, "alunos");
    cmd_condicao("id", TOKEN_IGUAL, num(5));
    ctx = (ContextoSelect){ alunos, &g_cmd, 0 };
    sim = registro_char(5, "Ana");
    nao = registro_char(6, "Bia");

    /* registro que casa: imprime e conta */
    captura_iniciar();
    r = callback_select(&sim, &ctx);
    saida = captura_finalizar();
    CHECK_INT(r, 0);                       /* 0 = continuar percorrendo */
    CHECK_INT(ctx.encontrados, 1);
    CHECK_STR(saida, "  5 | 'Ana'\n");

    /* registro que nao casa: nada impresso, contador intacto */
    captura_iniciar();
    r = callback_select(&nao, &ctx);
    saida = captura_finalizar();
    CHECK_INT(r, 0);
    CHECK_INT(ctx.encontrados, 1);
    CHECK_STR(saida, "");

    /* o contador acumula */
    captura_iniciar();
    callback_select(&sim, &ctx);
    captura_finalizar();
    CHECK_INT(ctx.encontrados, 2);
}

static void test_imprimir_tabela(void)
{
    const TabelaCat *alunos, *solo;
    const char *saida;
    char esperado[256];

    catalogo_padrao();
    alunos = catalogo_buscar_tabela(&g_cat, "alunos");
    solo   = catalogo_buscar_tabela(&g_cat, "solo");

    captura_iniciar();
    imprimir_tabela(alunos);
    saida = captura_finalizar();

    CHECK_CONTEM(saida, "Tabela alunos  (arquivo: alunos.dat)");
    snprintf(esperado, sizeof esperado, "registro: %d bytes (%d uteis + %d de preenchimento)",
             TAM_REGISTRO, TAM_INT + TAM_CHAR, TAM_REGISTRO - (TAM_INT + TAM_CHAR));
    CHECK_CONTEM(saida, esperado);
    CHECK_CONTEM(saida, "COLUNA");
    CHECK_CONTEM(saida, "id");
    CHECK_CONTEM(saida, "INT");
    CHECK_CONTEM(saida, "nome");
    CHECK_CONTEM(saida, "CHAR");
    snprintf(esperado, sizeof esperado, "0..%d", TAM_INT - 1);
    CHECK_CONTEM(saida, esperado);
    snprintf(esperado, sizeof esperado, "%d..%d", TAM_INT, TAM_INT + TAM_CHAR - 1);
    CHECK_CONTEM(saida, esperado);

    captura_iniciar();
    imprimir_tabela(solo);
    saida = captura_finalizar();
    CHECK_CONTEM(saida, "Tabela solo  (arquivo: solo.dat)");
    CHECK_NAO_CONTEM(saida, "CHAR");        /* so tem a coluna INT */
}

static void test_catalogo_imprimir(void)
{
    const char *saida;
    size_t n;

    /* vazio */
    catalogo_iniciar(&g_cat);
    captura_iniciar();
    catalogo_imprimir(&g_cat);
    saida = captura_finalizar();
    CHECK_CONTEM(saida, "=== CATALOGO (0 tabela(s)) ===");
    CHECK_CONTEM(saida, "(vazio)");

    /* com tabelas */
    catalogo_padrao();
    captura_iniciar();
    catalogo_imprimir(&g_cat);
    saida = captura_finalizar();
    CHECK_CONTEM(saida, "=== CATALOGO (3 tabela(s)) ===");
    CHECK_NAO_CONTEM(saida, "(vazio)");
    CHECK_CONTEM(saida, "Tabela alunos");
    CHECK_CONTEM(saida, "Tabela numeros");
    CHECK_CONTEM(saida, "Tabela solo");
    CHECK_CONTEM(saida, "\n\n");            /* linha em branco entre tabelas */

    /* nao deixa linha em branco depois da ultima tabela */
    n = strlen(saida);
    CHECK(n >= 2 && !(saida[n - 1] == '\n' && saida[n - 2] == '\n'));

    /* a ordem de insercao e preservada */
    CHECK(strstr(saida, "Tabela alunos") < strstr(saida, "Tabela numeros"));
    CHECK(strstr(saida, "Tabela numeros") < strstr(saida, "Tabela solo"));
}

/* ================================================================== */
/* Persistencia                                                        */
/* ================================================================== */

static void test_catalogo_salvar(void)
{
    char esperado[512], lido[1024];

    catalogo_padrao();
    remove("cat_salvar.cat");
    remove("cat_salvar.cat.tmp");

    CHECK_INT(catalogo_salvar(&g_cat, "cat_salvar.cat"), 1);

    snprintf(esperado, sizeof esperado,
             "%s %d\n"
             "TABELAS 3\n"
             "TABELA alunos 2\nCOLUNA id INT\nCOLUNA nome CHAR\n"
             "TABELA numeros 2\nCOLUNA id INT\nCOLUNA valor INT\n"
             "TABELA solo 1\nCOLUNA id INT\n",
             CAT_MAGICA, CAT_VERSAO);
    CHECK(ler_arquivo("cat_salvar.cat", lido, sizeof lido) >= 0);
    CHECK_STR(lido, esperado);

    /* o arquivo temporario nao sobra */
    CHECK(!existe_arquivo("cat_salvar.cat.tmp"));

    /* salvar de novo substitui o conteudo anterior */
    catalogo_iniciar(&g_cat);
    CHECK_INT(catalogo_salvar(&g_cat, "cat_salvar.cat"), 1);
    snprintf(esperado, sizeof esperado, "%s %d\nTABELAS 0\n", CAT_MAGICA, CAT_VERSAO);
    ler_arquivo("cat_salvar.cat", lido, sizeof lido);
    CHECK_STR(lido, esperado);
    CHECK(!existe_arquivo("cat_salvar.cat.tmp"));
}

static void test_catalogo_salvar_falhas(void)
{
    catalogo_padrao();

    /* diretorio de destino inexistente: nao da nem para criar o temporario */
    CHECK_INT(catalogo_salvar(&g_cat, "pasta_que_nao_existe/cat.cat"), 0);

    /* destino e um diretorio: o rename falha e o temporario e removido */
    mkdir("destino_dir", 0700);
    CHECK_INT(catalogo_salvar(&g_cat, "destino_dir"), 0);
    CHECK(!existe_arquivo("destino_dir.tmp"));
    rmdir("destino_dir");
}

static void test_ler_palavra(void)
{
    char buf[16];
    FILE *f;

    f = abrir_texto("alunos   id\nnome");
    CHECK_INT(ler_palavra(f, buf, sizeof buf), 1);
    CHECK_STR(buf, "alunos");
    CHECK_INT(ler_palavra(f, buf, sizeof buf), 1);     /* pula espacos */
    CHECK_STR(buf, "id");
    CHECK_INT(ler_palavra(f, buf, sizeof buf), 1);     /* pula quebra de linha */
    CHECK_STR(buf, "nome");
    CHECK_INT(ler_palavra(f, buf, sizeof buf), 0);     /* fim do arquivo */
    fclose(f);

    /* a palavra precisa caber, incluindo o '\0' */
    f = abrir_texto("abcde");
    CHECK_INT(ler_palavra(f, buf, 6), 1);
    CHECK_STR(buf, "abcde");
    fclose(f);

    f = abrir_texto("abcde");
    strcpy(buf, "intacto");
    CHECK_INT(ler_palavra(f, buf, 5), 0);
    CHECK_STR(buf, "intacto");                         /* saida nao e alterada */
    fclose(f);

    /* arquivo vazio e arquivo so com espacos */
    f = abrir_texto("");
    CHECK_INT(ler_palavra(f, buf, sizeof buf), 0);
    fclose(f);

    f = abrir_texto("  \n\t ");
    CHECK_INT(ler_palavra(f, buf, sizeof buf), 0);
    fclose(f);
}

static void test_ler_inteiro(void)
{
    FILE *f;
    int v = 99;

    f = abrir_texto("42 -7\n0");
    CHECK_INT(ler_inteiro(f, &v), 1);
    CHECK_INT(v, 42);
    CHECK_INT(ler_inteiro(f, &v), 1);
    CHECK_INT(v, -7);
    CHECK_INT(ler_inteiro(f, &v), 1);
    CHECK_INT(v, 0);
    CHECK_INT(ler_inteiro(f, &v), 0);                  /* fim do arquivo */
    fclose(f);

    v = 99;
    f = abrir_texto("abc");
    CHECK_INT(ler_inteiro(f, &v), 0);
    CHECK_INT(v, 99);
    fclose(f);

    f = abrir_texto("");
    CHECK_INT(ler_inteiro(f, &v), 0);
    fclose(f);
}

static void test_carregar_de_arquivo(void)
{
    FILE *f;

    /* arquivo valido */
    catalogo_iniciar(&g_cat);
    f = abrir_texto("CATALOGO 1\nTABELAS 1\nTABELA t 2\nCOLUNA id INT\nCOLUNA nome CHAR\n");
    CHECK_INT(carregar_de_arquivo(&g_cat, f), 1);
    fclose(f);
    CHECK_INT(g_cat.numero_tabelas, 1);
    CHECK_STR(g_cat.tabelas[0].nome, "t");

    /* arquivo invalido: devolve 0 com mensagem (quem desfaz o parcial e
     * catalogo_carregar) */
    catalogo_iniciar(&g_cat);
    f = abrir_texto("CATALOGO 1\nTABELAS 2\n"
                    "TABELA a 1\nCOLUNA id INT\n"
                    "TABELA b 1\nCOLUNA id FLOAT\n");
    CHECK_INT(carregar_de_arquivo(&g_cat, f), 0);
    fclose(f);
    CHECK_CONTEM(g_cat.mensagem, "tipo 'FLOAT' desconhecido");
    CHECK_INT(g_cat.numero_tabelas, 1);                /* 'a' ja tinha entrado */
}

static void test_catalogo_carregar_ida_e_volta(void)
{
    catalogo_padrao();
    CHECK_INT(catalogo_salvar(&g_cat, "cat_volta.cat"), 1);

    CHECK_INT(catalogo_carregar(&g_cat2, "cat_volta.cat"), 1);
    CHECK_INT(g_cat2.numero_tabelas, g_cat.numero_tabelas);

    for (int i = 0; i < g_cat.numero_tabelas; i++) {
        const TabelaCat *a = &g_cat.tabelas[i], *b = &g_cat2.tabelas[i];
        CHECK_STR(b->nome, a->nome);
        CHECK_STR(b->arquivo, a->arquivo);
        CHECK_INT(b->numero_colunas, a->numero_colunas);
        CHECK_INT(b->tamanho_util, a->tamanho_util);
        CHECK_INT(b->tamanho_registro, a->tamanho_registro);
        for (int j = 0; j < a->numero_colunas; j++) {
            CHECK_STR(b->colunas[j].nome, a->colunas[j].nome);
            CHECK(b->colunas[j].tipo == a->colunas[j].tipo);
            CHECK_INT(b->colunas[j].tamanho, a->colunas[j].tamanho);
            CHECK_INT(b->colunas[j].deslocamento, a->colunas[j].deslocamento);
        }
    }

    /* catalogo vazio tambem faz a viagem de ida e volta */
    catalogo_iniciar(&g_cat);
    CHECK_INT(catalogo_salvar(&g_cat, "cat_volta.cat"), 1);
    CHECK_INT(catalogo_carregar(&g_cat2, "cat_volta.cat"), 1);
    CHECK_INT(g_cat2.numero_tabelas, 0);
}

static void test_catalogo_carregar_casos_especiais(void)
{
    catalogo_padrao();
    remove("nao_existe.cat");

    /* arquivo inexistente: 0 e o catalogo (mesmo preenchido) vira vazio */
    CHECK_INT(catalogo_carregar(&g_cat, "nao_existe.cat"), 0);
    CHECK_INT(g_cat.numero_tabelas, 0);

    /* carregar substitui o que ja estava na memoria */
    catalogo_padrao();
    escrever_arquivo("so_uma.cat",
                     "CATALOGO 1\nTABELAS 1\nTABELA unica 1\nCOLUNA id INT\n");
    CHECK_INT(catalogo_carregar(&g_cat, "so_uma.cat"), 1);
    CHECK_INT(g_cat.numero_tabelas, 1);
    CHECK_STR(g_cat.tabelas[0].nome, "unica");
    CHECK(catalogo_buscar_tabela(&g_cat, "alunos") == NULL);

    /* o formato aceita qualquer espacamento entre as palavras */
    escrever_arquivo("espacos.cat",
                     "CATALOGO   1   TABELAS 1\n\n  TABELA   t   1 COLUNA id   INT");
    CHECK_INT(catalogo_carregar(&g_cat, "espacos.cat"), 1);
    CHECK_INT(g_cat.numero_tabelas, 1);

    /* catalogo vazio valido */
    escrever_arquivo("vazio.cat", "CATALOGO 1\nTABELAS 0\n");
    CHECK_INT(catalogo_carregar(&g_cat, "vazio.cat"), 1);
    CHECK_INT(g_cat.numero_tabelas, 0);

    /* exatamente MAX_TABELAS tabelas e aceito */
    {
        char *conteudo = malloc(MAX_TABELAS * 64 + 64);
        size_t pos = 0;
        pos += (size_t)sprintf(conteudo + pos, "CATALOGO 1\nTABELAS %d\n", MAX_TABELAS);
        for (int i = 0; i < MAX_TABELAS; i++)
            pos += (size_t)sprintf(conteudo + pos, "TABELA t%d 1\nCOLUNA id INT\n", i);
        escrever_arquivo("cheio.cat", conteudo);
        free(conteudo);
        CHECK_INT(catalogo_carregar(&g_cat, "cheio.cat"), 1);
        CHECK_INT(g_cat.numero_tabelas, MAX_TABELAS);
    }
}

static void esperar_carga_invalida(int linha, const char *conteudo, const char *trecho)
{
    int falhas_antes = g_falhas;

    catalogo_padrao();                       /* garante que o parcial e descartado */
    escrever_arquivo("invalido.cat", conteudo);

    CHECK_INT(catalogo_carregar(&g_cat, "invalido.cat"), -1);
    CHECK_INT(g_cat.numero_tabelas, 0);
    CHECK_CONTEM(g_cat.mensagem, trecho);

    if (g_falhas != falhas_antes)
        printf("      (chamada em test_catalog.c:%d)\n", linha);
}

#define ESPERAR_INVALIDO(conteudo, trecho) \
    esperar_carga_invalida(__LINE__, conteudo, trecho)

static void test_catalogo_carregar_invalido(void)
{
    char buf[1024], nome_longo[MAX_LEXEMA + 1];

    /* cabecalho */
    ESPERAR_INVALIDO("", "cabecalho invalido");
    ESPERAR_INVALIDO("OUTRO 1\nTABELAS 0\n", "cabecalho invalido");
    ESPERAR_INVALIDO("catalogo 1\nTABELAS 0\n", "cabecalho invalido");  /* case-sensitive */

    /* versao */
    ESPERAR_INVALIDO("CATALOGO 2\nTABELAS 0\n", "versao");
    ESPERAR_INVALIDO("CATALOGO 0\nTABELAS 0\n", "versao");
    ESPERAR_INVALIDO("CATALOGO x\nTABELAS 0\n", "versao");
    ESPERAR_INVALIDO("CATALOGO", "versao");

    /* numero de tabelas */
    ESPERAR_INVALIDO("CATALOGO 1\n", "numero de tabelas invalido");
    ESPERAR_INVALIDO("CATALOGO 1\nTABELA 0\n", "numero de tabelas invalido");
    ESPERAR_INVALIDO("CATALOGO 1\nTABELAS -1\n", "numero de tabelas invalido");
    ESPERAR_INVALIDO("CATALOGO 1\nTABELAS x\n", "numero de tabelas invalido");
    snprintf(buf, sizeof buf, "CATALOGO 1\nTABELAS %d\n", MAX_TABELAS + 1);
    ESPERAR_INVALIDO(buf, "numero de tabelas invalido");

    /* entrada TABELA */
    ESPERAR_INVALIDO("CATALOGO 1\nTABELAS 1\n", "entrada TABELA 1 invalida");
    ESPERAR_INVALIDO("CATALOGO 1\nTABELAS 1\nTABELAX t 1\nCOLUNA id INT\n",
                     "entrada TABELA 1 invalida");
    ESPERAR_INVALIDO("CATALOGO 1\nTABELAS 1\nTABELA t 0\n", "entrada TABELA 1 invalida");
    ESPERAR_INVALIDO("CATALOGO 1\nTABELAS 1\nTABELA t x\n", "entrada TABELA 1 invalida");
    snprintf(buf, sizeof buf, "CATALOGO 1\nTABELAS 1\nTABELA t %d\n", MAX_COLUNAS + 1);
    ESPERAR_INVALIDO(buf, "entrada TABELA 1 invalida");

    /* nome que nao cabe em MAX_LEXEMA bytes */
    repetir(nome_longo, 'a', MAX_LEXEMA);
    snprintf(buf, sizeof buf, "CATALOGO 1\nTABELAS 1\nTABELA %s 1\nCOLUNA id INT\n", nome_longo);
    ESPERAR_INVALIDO(buf, "entrada TABELA 1 invalida");

    /* a segunda TABELA e a que esta quebrada */
    ESPERAR_INVALIDO("CATALOGO 1\nTABELAS 2\n"
                     "TABELA a 1\nCOLUNA id INT\n"
                     "TABELA b\n",
                     "entrada TABELA 2 invalida");

    /* entrada COLUNA */
    ESPERAR_INVALIDO("CATALOGO 1\nTABELAS 1\nTABELA alunos 1\nCAMPO id INT\n",
                     "entrada COLUNA 1 invalida");
    ESPERAR_INVALIDO("CATALOGO 1\nTABELAS 1\nTABELA alunos 1\nCOLUNA id\n",
                     "entrada COLUNA 1 invalida");
    ESPERAR_INVALIDO("CATALOGO 1\nTABELAS 1\nTABELA alunos 2\nCOLUNA id INT\n",
                     "entrada COLUNA 2 invalida");   /* arquivo truncado */
    ESPERAR_INVALIDO("CATALOGO 1\nTABELAS 1\nTABELA alunos 1\nCAMPO id INT\n",
                     "'alunos'");                    /* cita a tabela */

    /* tipo desconhecido (so INT e CHAR; diferencia maiusculas) */
    ESPERAR_INVALIDO("CATALOGO 1\nTABELAS 1\nTABELA t 1\nCOLUNA id FLOAT\n",
                     "tipo 'FLOAT' desconhecido");
    ESPERAR_INVALIDO("CATALOGO 1\nTABELAS 1\nTABELA t 1\nCOLUNA id int\n",
                     "tipo 'int' desconhecido");

    /* regras de catalogo_adicionar_tabela tambem valem ao carregar */
    ESPERAR_INVALIDO("CATALOGO 1\nTABELAS 1\nTABELA 1x 1\nCOLUNA id INT\n",
                     "nome de tabela invalido");
    ESPERAR_INVALIDO("CATALOGO 1\nTABELAS 1\nTABELA t 2\nCOLUNA id INT\nCOLUNA ID CHAR\n",
                     "duplicada");
    ESPERAR_INVALIDO("CATALOGO 1\nTABELAS 1\nTABELA t 1\nCOLUNA nome CHAR\n",
                     "arvore B+");
    ESPERAR_INVALIDO("CATALOGO 1\nTABELAS 2\n"
                     "TABELA a 1\nCOLUNA id INT\n"
                     "TABELA A 1\nCOLUNA id INT\n",
                     "ja existe");

    /* erro na 2a tabela: a 1a, ja valida, tambem e descartada */
    ESPERAR_INVALIDO("CATALOGO 1\nTABELAS 2\n"
                     "TABELA a 1\nCOLUNA id INT\n"
                     "TABELA b 1\nCOLUNA id FLOAT\n",
                     "tipo 'FLOAT' desconhecido");
    CHECK(catalogo_buscar_tabela(&g_cat, "a") == NULL);
}

/* ================================================================== */
/* Execucao de comandos (integracao com Cache / Pager / Arvore B+)     */
/* ================================================================== */

static void remover_arquivos_de_dados(void)
{
    remove("alunos.dat");
    remove("numeros.dat");
    remove("solo.dat");
    remove("notas.dat");
}

/* Executa um SELECT e devolve o que foi impresso; *ok recebe o retorno */
static const char *rodar_select(int *ok)
{
    const char *saida;
    captura_iniciar();
    *ok = catalogo_executar(&g_cat, &g_cmd);
    saida = captura_finalizar();
    return saida;
}

static void inserir_aluno(int id, const char *nome, int esperado)
{
    cmd_iniciar(CMD_INSERT, "alunos");
    cmd_valor(num(id));
    cmd_valor(txt(nome));
    CHECK_INT(catalogo_executar(&g_cat, &g_cmd), esperado);
}

static void test_selecionar_arquivo_tabela(void)
{
    const TabelaCat *t;

    remover_arquivos_de_dados();
    catalogo_padrao();

    t = catalogo_buscar_tabela(&g_cat, "alunos");
    CHECK_INT(selecionar_arquivo_tabela(&g_cat, t), 1);

    /* da para trocar de tabela quantas vezes quiser */
    t = catalogo_buscar_tabela(&g_cat, "numeros");
    CHECK_INT(selecionar_arquivo_tabela(&g_cat, t), 1);
    t = catalogo_buscar_tabela(&g_cat, "alunos");
    CHECK_INT(selecionar_arquivo_tabela(&g_cat, t), 1);

    remover_arquivos_de_dados();
}

static void test_executar_create(void)
{
    int antes;

    remover_arquivos_de_dados();
    catalogo_iniciar(&g_cat);

    /* sucesso: registra a tabela e cria o arquivo de dados */
    cmd_iniciar(CMD_CREATE, "alunos");
    cmd_coluna("id", TOKEN_INT);
    cmd_coluna("nome", TOKEN_CHAR);
    CHECK_INT(catalogo_executar(&g_cat, &g_cmd), 1);
    CHECK_INT(g_cat.numero_tabelas, 1);
    CHECK(catalogo_buscar_tabela(&g_cat, "alunos") != NULL);
    CHECK(existe_arquivo("alunos.dat"));

    /* duplicada */
    antes = g_cat.numero_tabelas;
    CHECK_INT(catalogo_executar(&g_cat, &g_cmd), 0);
    CHECK_CONTEM(g_cat.mensagem, "ja existe");
    CHECK_INT(g_cat.numero_tabelas, antes);

    /* nome invalido */
    cmd_iniciar(CMD_CREATE, "1x");
    cmd_coluna("id", TOKEN_INT);
    CHECK_INT(catalogo_executar(&g_cat, &g_cmd), 0);
    CHECK_CONTEM(g_cat.mensagem, "nome de tabela invalido");

    /* restricao da arvore B+ */
    cmd_iniciar(CMD_CREATE, "notas");
    cmd_coluna("nome", TOKEN_CHAR);
    CHECK_INT(catalogo_executar(&g_cat, &g_cmd), 0);
    CHECK_CONTEM(g_cat.mensagem, "arvore B+");
    CHECK_INT(g_cat.numero_tabelas, antes);
    CHECK(!existe_arquivo("notas.dat"));

    /* segunda tabela */
    cmd_iniciar(CMD_CREATE, "numeros");
    cmd_coluna("id", TOKEN_INT);
    cmd_coluna("valor", TOKEN_INT);
    CHECK_INT(catalogo_executar(&g_cat, &g_cmd), 1);
    CHECK_INT(g_cat.numero_tabelas, 2);
    CHECK(existe_arquivo("numeros.dat"));

    remover_arquivos_de_dados();
}

static void test_executar_comando_desconhecido(void)
{
    catalogo_iniciar(&g_cat);
    cmd_iniciar(CMD_SELECT, "x");
    g_cmd.tipo = (TipoCmd)999;

    CHECK_INT(catalogo_executar(&g_cat, &g_cmd), 0);
    CHECK_CONTEM(g_cat.mensagem, "comando desconhecido");
}

static void test_executar_insert(void)
{
    remover_arquivos_de_dados();
    catalogo_iniciar(&g_cat);

    cmd_iniciar(CMD_CREATE, "alunos");
    cmd_coluna("id", TOKEN_INT);
    cmd_coluna("nome", TOKEN_CHAR);
    CHECK_INT(catalogo_executar(&g_cat, &g_cmd), 1);

    /* inserts validos (inclusive CHAR vazio e CHAR com o tamanho maximo) */
    inserir_aluno(1, "Ana", 1);
    inserir_aluno(2, "Bia", 1);
    inserir_aluno(3, "Caio", 1);
    inserir_aluno(4, "", 1);

    /* chave duplicada: erro e o registro original e preservado */
    inserir_aluno(1, "Zed", 0);
    CHECK_CONTEM(g_cat.mensagem, "chave duplicada");
    CHECK_CONTEM(g_cat.mensagem, "'id'");
    CHECK_CONTEM(g_cat.mensagem, "'alunos'");

    /* tabela inexistente */
    cmd_iniciar(CMD_INSERT, "fantasma");
    cmd_valor(num(1));
    CHECK_INT(catalogo_executar(&g_cat, &g_cmd), 0);
    CHECK_CONTEM(g_cat.mensagem, "nao existe");

    /* quantidade de valores errada */
    cmd_iniciar(CMD_INSERT, "alunos");
    cmd_valor(num(10));
    CHECK_INT(catalogo_executar(&g_cat, &g_cmd), 0);
    CHECK_CONTEM(g_cat.mensagem, "espera 2 valor(es)");

    /* tipo errado */
    cmd_iniciar(CMD_INSERT, "alunos");
    cmd_valor(txt("x")); cmd_valor(txt("Ana"));
    CHECK_INT(catalogo_executar(&g_cat, &g_cmd), 0);
    CHECK_CONTEM(g_cat.mensagem, "e INT");

    /* CHAR grande demais e barrado antes de tocar na arvore */
    inserir_aluno(11, "abcdefgh", 0);
    CHECK_CONTEM(g_cat.mensagem, "comporta");

    /* nenhuma das tentativas falhas gravou nada: continuam 4 registros */
    {
        int ok;
        const char *saida;
        cmd_iniciar(CMD_SELECT, "alunos");
        saida = rodar_select(&ok);
        CHECK_INT(ok, 1);
        CHECK_CONTEM(saida, "4 registro(s) encontrado(s).");
        CHECK_NAO_CONTEM(saida, "Zed");
    }

    remover_arquivos_de_dados();
}

static void test_executar_select(void)
{
    int ok;
    const char *saida;

    remover_arquivos_de_dados();
    catalogo_iniciar(&g_cat);

    cmd_iniciar(CMD_CREATE, "alunos");
    cmd_coluna("id", TOKEN_INT);
    cmd_coluna("nome", TOKEN_CHAR);
    CHECK_INT(catalogo_executar(&g_cat, &g_cmd), 1);
    inserir_aluno(1, "Ana", 1);
    inserir_aluno(2, "Bia", 1);
    inserir_aluno(3, "Caio", 1);
    inserir_aluno(4, "", 1);

    /* sem WHERE: cabecalho, todos os registros e o total */
    cmd_iniciar(CMD_SELECT, "alunos");
    saida = rodar_select(&ok);
    CHECK_INT(ok, 1);
    CHECK_CONTEM(saida, "Resultado de SELECT em 'alunos':");
    CHECK_CONTEM(saida, "id | nome");
    CHECK_CONTEM(saida, "  1 | 'Ana'");
    CHECK_CONTEM(saida, "  2 | 'Bia'");
    CHECK_CONTEM(saida, "  3 | 'Caio'");
    CHECK_CONTEM(saida, "  4 | ''");
    CHECK_CONTEM(saida, "4 registro(s) encontrado(s).");
    CHECK_NAO_CONTEM(saida, "nenhum registro");

    /* nome da tabela sem diferenciar maiusculas; o titulo usa a grafia do catalogo */
    cmd_iniciar(CMD_SELECT, "ALUNOS");
    saida = rodar_select(&ok);
    CHECK_INT(ok, 1);
    CHECK_CONTEM(saida, "Resultado de SELECT em 'alunos':");

    /* filtro pela chave */
    cmd_iniciar(CMD_SELECT, "alunos");
    cmd_condicao("id", TOKEN_MAIOR, num(1));
    saida = rodar_select(&ok);
    CHECK_INT(ok, 1);
    CHECK_CONTEM(saida, "3 registro(s) encontrado(s).");
    CHECK_NAO_CONTEM(saida, "'Ana'");

    /* filtro por CHAR */
    cmd_iniciar(CMD_SELECT, "alunos");
    cmd_condicao("nome", TOKEN_IGUAL, txt("Bia"));
    saida = rodar_select(&ok);
    CHECK_INT(ok, 1);
    CHECK_CONTEM(saida, "1 registro(s) encontrado(s).");
    CHECK_CONTEM(saida, "  2 | 'Bia'");
    CHECK_NAO_CONTEM(saida, "'Ana'");

    cmd_iniciar(CMD_SELECT, "alunos");
    cmd_condicao("nome", TOKEN_IGUAL, txt(""));
    saida = rodar_select(&ok);
    CHECK_INT(ok, 1);
    CHECK_CONTEM(saida, "1 registro(s) encontrado(s).");
    CHECK_CONTEM(saida, "  4 | ''");

    /* condicoes combinadas (AND) */
    cmd_iniciar(CMD_SELECT, "alunos");
    cmd_condicao("id", TOKEN_MAIOR_IGUAL, num(2));
    cmd_condicao("nome", TOKEN_DIFERENTE, txt("Bia"));
    saida = rodar_select(&ok);
    CHECK_INT(ok, 1);
    CHECK_CONTEM(saida, "2 registro(s) encontrado(s).");
    CHECK_CONTEM(saida, "'Caio'");
    CHECK_NAO_CONTEM(saida, "'Bia'");
    CHECK_NAO_CONTEM(saida, "'Ana'");

    /* nenhum resultado e um sucesso (devolve 1) */
    cmd_iniciar(CMD_SELECT, "alunos");
    cmd_condicao("id", TOKEN_MAIOR, num(100));
    saida = rodar_select(&ok);
    CHECK_INT(ok, 1);
    CHECK_CONTEM(saida, "(nenhum registro encontrado)");
    CHECK_CONTEM(saida, "0 registro(s) encontrado(s).");

    /* tabela inexistente: erro e nada impresso */
    cmd_iniciar(CMD_SELECT, "fantasma");
    saida = rodar_select(&ok);
    CHECK_INT(ok, 0);
    CHECK_STR(saida, "");
    CHECK_CONTEM(g_cat.mensagem, "nao existe");

    /* coluna inexistente */
    cmd_iniciar(CMD_SELECT, "alunos");
    cmd_condicao("idade", TOKEN_IGUAL, num(1));
    saida = rodar_select(&ok);
    CHECK_INT(ok, 0);
    CHECK_STR(saida, "");
    CHECK_CONTEM(g_cat.mensagem, "'idade'");

    /* tipo incompativel na condicao */
    cmd_iniciar(CMD_SELECT, "alunos");
    cmd_condicao("id", TOKEN_IGUAL, txt("x"));
    saida = rodar_select(&ok);
    CHECK_INT(ok, 0);
    CHECK_STR(saida, "");
    CHECK_CONTEM(g_cat.mensagem, "WHERE");

    remover_arquivos_de_dados();
}

static void test_executar_varias_tabelas(void)
{
    int ok;
    const char *saida;

    remover_arquivos_de_dados();
    catalogo_iniciar(&g_cat);

    cmd_iniciar(CMD_CREATE, "alunos");
    cmd_coluna("id", TOKEN_INT); cmd_coluna("nome", TOKEN_CHAR);
    CHECK_INT(catalogo_executar(&g_cat, &g_cmd), 1);
    cmd_iniciar(CMD_CREATE, "numeros");
    cmd_coluna("id", TOKEN_INT); cmd_coluna("valor", TOKEN_INT);
    CHECK_INT(catalogo_executar(&g_cat, &g_cmd), 1);
    cmd_iniciar(CMD_CREATE, "solo");
    cmd_coluna("id", TOKEN_INT);
    CHECK_INT(catalogo_executar(&g_cat, &g_cmd), 1);

    inserir_aluno(1, "Ana", 1);

    cmd_iniciar(CMD_INSERT, "numeros");
    cmd_valor(num(10)); cmd_valor(num(100));
    CHECK_INT(catalogo_executar(&g_cat, &g_cmd), 1);
    cmd_iniciar(CMD_INSERT, "numeros");
    cmd_valor(num(20)); cmd_valor(num(-5));
    CHECK_INT(catalogo_executar(&g_cat, &g_cmd), 1);

    cmd_iniciar(CMD_INSERT, "solo");
    cmd_valor(num(7));
    CHECK_INT(catalogo_executar(&g_cat, &g_cmd), 1);

    /* a mesma chave pode existir em tabelas diferentes */
    inserir_aluno(10, "Bia", 1);

    /* numeros: coluna INT com filtro e valor negativo */
    cmd_iniciar(CMD_SELECT, "numeros");
    saida = rodar_select(&ok);
    CHECK_INT(ok, 1);
    CHECK_CONTEM(saida, "id | valor");
    CHECK_CONTEM(saida, "  10 | 100");
    CHECK_CONTEM(saida, "  20 | -5");
    CHECK_CONTEM(saida, "2 registro(s) encontrado(s).");
    CHECK_NAO_CONTEM(saida, "'Ana'");

    cmd_iniciar(CMD_SELECT, "numeros");
    cmd_condicao("valor", TOKEN_MENOR, num(0));
    saida = rodar_select(&ok);
    CHECK_INT(ok, 1);
    CHECK_CONTEM(saida, "1 registro(s) encontrado(s).");
    CHECK_CONTEM(saida, "  20 | -5");

    /* solo: so a coluna id */
    cmd_iniciar(CMD_SELECT, "solo");
    saida = rodar_select(&ok);
    CHECK_INT(ok, 1);
    CHECK_CONTEM(saida, "  7\n");
    CHECK_CONTEM(saida, "1 registro(s) encontrado(s).");

    /* alunos nao foi afetada pelos inserts nas outras tabelas */
    cmd_iniciar(CMD_SELECT, "alunos");
    saida = rodar_select(&ok);
    CHECK_INT(ok, 1);
    CHECK_CONTEM(saida, "2 registro(s) encontrado(s).");
    CHECK_CONTEM(saida, "'Ana'");
    CHECK_CONTEM(saida, "'Bia'");
    CHECK_NAO_CONTEM(saida, "100");

    remover_arquivos_de_dados();
}

/* ================================================================== */
/* main                                                                */
/* ================================================================== */

int main(void)
{
    char origem[1024];
    char dir[] = "/tmp/test_catalog_XXXXXX";
    char comando[256];

    if (!getcwd(origem, sizeof origem) || !mkdtemp(dir)) {
        perror("preparando diretorio de teste");
        return 2;
    }
    if (chdir(dir) != 0) {
        perror("chdir");
        return 2;
    }

    printf("Testes de Catalog.c (diretorio de trabalho: %s)\n\n", dir);

    /* utilitarios */
    RODAR(test_erro_cat);
    RODAR(test_tamanho_do_tipo);
    RODAR(test_nome_valido);
    RODAR(test_montar_nome_arquivo);

    /* consulta ao catalogo */
    RODAR(test_catalogo_iniciar);
    RODAR(test_catalogo_buscar_tabela);
    RODAR(test_tabela_buscar_coluna);

    /* CREATE TABLE */
    RODAR(test_adicionar_tabela_sucesso);
    RODAR(test_adicionar_tabela_nome_e_limites);
    RODAR(test_adicionar_tabela_colunas_invalidas);
    RODAR(test_adicionar_tabela_restricoes_fisicas);

    /* analise semantica */
    RODAR(test_valor_compativel);
    RODAR(test_validar_insert);
    RODAR(test_validar_select);

    /* empacotamento de CHAR */
    RODAR(test_empacotar_char);
    RODAR(test_desempacotar_char);
    RODAR(test_empacotar_desempacotar_ida_e_volta);

    /* comparacoes e filtro */
    RODAR(test_comparar_int);
    RODAR(test_comparar_char);
    RODAR(test_registro_corresponde);

    /* impressao */
    RODAR(test_imprimir_registro);
    RODAR(test_callback_select);
    RODAR(test_imprimir_tabela);
    RODAR(test_catalogo_imprimir);

    /* persistencia do catalogo */
    RODAR(test_catalogo_salvar);
    RODAR(test_catalogo_salvar_falhas);
    RODAR(test_ler_palavra);
    RODAR(test_ler_inteiro);
    RODAR(test_carregar_de_arquivo);
    RODAR(test_catalogo_carregar_ida_e_volta);
    RODAR(test_catalogo_carregar_casos_especiais);
    RODAR(test_catalogo_carregar_invalido);

    /* execucao de comandos (usa Cache / Pager / Arvore B+ de verdade) */
    RODAR(test_selecionar_arquivo_tabela);
    RODAR(test_executar_create);
    RODAR(test_executar_comando_desconhecido);
    RODAR(test_executar_insert);
    RODAR(test_executar_select);
    RODAR(test_executar_varias_tabelas);

    printf("\n%d verificacao(oes), %d falha(s)\n", g_verificacoes, g_falhas);
    printf("%s\n", g_falhas == 0 ? "TODOS OS TESTES PASSARAM" : "HA TESTES FALHANDO");

    /* limpa o diretorio temporario */
    if (chdir(origem) == 0) {
        snprintf(comando, sizeof comando, "rm -rf '%s'", dir);
        if (system(comando) != 0)
            fprintf(stderr, "aviso: nao foi possivel remover %s\n", dir);
    }

    return g_falhas == 0 ? 0 : 1;
}
