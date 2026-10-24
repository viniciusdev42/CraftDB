#define _POSIX_C_SOURCE 200809L
/*
 * Catalogo do banco de dados relacional (dicionario de dados).
 *
 * O catalogo guarda os METADADOS do banco: quais tabelas existem, quais
 * colunas cada uma tem, o tipo (INT/CHAR), o tamanho em bytes e a posicao
 * (deslocamento) de cada coluna dentro do registro de tamanho fixo.
 *
 * Ele e a ponte entre o Parser e o futuro motor de armazenamento:
 *
 *   texto SQL -> Tokenizer -> Parser (AST) -> CATALOGO (analise semantica)
 *
 *   CREATE TABLE : registra a tabela no catalogo (e grava em disco)
 *   INSERT       : confere tabela, quantidade e tipos dos valores
 *   SELECT       : confere tabela, colunas do WHERE e tipos dos literais
 *
 * Layout do registro (fixo em TAM_REGISTRO = 8 bytes):
 *
 *   INT  ocupa TAM_INT  bytes
 *   CHAR ocupa TAM_CHAR bytes (texto de ate TAM_CHAR caracteres)
 *
 *   Exemplo: alunos (id INT, nome CHAR)
 *     bytes 0..3 -> id      bytes 4..7 -> nome
 *
 *   A soma dos tamanhos das colunas nao pode passar de TAM_REGISTRO.
 *   Se sobrar espaco, o restante do registro e preenchimento (padding).
 *
 * Persistencia: arquivo texto "catalogo.cat" (veja ARQUIVO_CATALOGO).
 * So nome e tipo das colunas sao gravados; tamanhos e deslocamentos sao
 * recalculados ao carregar, pelas mesmas regras usadas no CREATE TABLE.
 
 *
 * Uso:
 *   ./catalogo                      executa um exemplo embutido
 *   ./catalogo script.sql           executa os comandos do arquivo
 *   ./catalogo script.sql meu.cat   usa outro arquivo de catalogo
 */

/* O Parser traz funcoes auxiliares que este programa nao usa (ex.: imprimir) */
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

/* Reaproveita Parser.c (que ja inclui Tokenizer.c) sem o main() dele */
#define PARSER_CATALOG
#include "Parser.c"

/* Motor de armazenamento integrado: Pager -> Cache -> Arvore B+. */
#include "Pager.c"
#include "Cache.c"
#include "B_Plus_Tree.c"

/* ------------------------------------------------------------------ */
/* Constantes                                                          */
/* ------------------------------------------------------------------ */

#define MAX_TABELAS       32                /* tabelas no catalogo          */
#define TAM_INT           4                 /* bytes de uma coluna INT      */
#define TAM_CHAR          4                 /* bytes de uma coluna CHAR     */
#define TAM_ARQUIVO       (MAX_LEXEMA + 5)  /* "nome" + ".dat" + '\0'       */
#define ARQUIVO_CATALOGO  "catalogo.cat"

#define CAT_MAGICA        "CATALOGO"        /* primeira palavra do arquivo  */
#define CAT_VERSAO        1

/* ------------------------------------------------------------------ */
/* Estruturas do catalogo                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    char      nome[MAX_LEXEMA];
    TipoToken tipo;                 /* TOKEN_INT ou TOKEN_CHAR              */
    int       tamanho;              /* bytes ocupados no registro           */
    int       deslocamento;         /* posicao da coluna dentro do registro */
} ColunaCat;

typedef struct {
    char      nome[MAX_LEXEMA];
    char      arquivo[TAM_ARQUIVO]; /* arquivo de dados da tabela (nome.dat)*/
    ColunaCat colunas[MAX_COLUNAS];
    int       numero_colunas;
    int       tamanho_util;         /* soma dos tamanhos das colunas        */
    int       tamanho_registro;     /* sempre TAM_REGISTRO                  */
} TabelaCat;

typedef struct {
    TabelaCat tabelas[MAX_TABELAS];
    int       numero_tabelas;
    char      mensagem[MAX_MESSAGEM];   /* ultimo erro semantico            */
} Catalogo;

/* ------------------------------------------------------------------ */
/* Utilitarios                                                         */
/* ------------------------------------------------------------------ */

/* Grava a mensagem de erro no catalogo. Devolve sempre 0. */
static int erro_cat(Catalogo *cat, const char *formato, ...)
{
    va_list ap;
    va_start(ap, formato);
    vsnprintf(cat->mensagem, MAX_MESSAGEM, formato, ap);
    va_end(ap);
    return 0;
}

static int tamanho_do_tipo(TipoToken tipo)
{
    return (tipo == TOKEN_INT) ? TAM_INT : TAM_CHAR;
}

/* Identificador valido: letra ou '_' seguido de letras, digitos ou '_' */
static int nome_valido(const char *nome)
{
    if (!(isalpha((unsigned char)nome[0]) || nome[0] == '_'))
        return 0;
    for (size_t i = 1; nome[i]; i++)
        if (!(isalnum((unsigned char)nome[i]) || nome[i] == '_'))
            return 0;
    return 1;
}

/* "Alunos" -> "alunos.dat" */
static void montar_nome_arquivo(char *saida, const char *tabela)
{
    size_t i = 0;
    for (; tabela[i] && i < MAX_LEXEMA - 1; i++)
        saida[i] = (char)tolower((unsigned char)tabela[i]);
    saida[i] = '\0';
    strcat(saida, ".dat");
}

/* ------------------------------------------------------------------ */
/* Consulta ao catalogo                                                */
/* Nomes de tabelas e colunas NAO diferenciam maiusculas/minusculas.   */
/* ------------------------------------------------------------------ */

static void catalogo_iniciar(Catalogo *cat)
{
    memset(cat, 0, sizeof *cat);
}

static const TabelaCat *catalogo_buscar_tabela(const Catalogo *cat, const char *nome)
{
    for (int i = 0; i < cat->numero_tabelas; i++)
        if (contexto_igual(cat->tabelas[i].nome, nome))
            return &cat->tabelas[i];
    return NULL;
}

/* Devolve o indice da coluna na tabela, ou -1 se nao existir */
static int tabela_buscar_coluna(const TabelaCat *tabela, const char *nome)
{
    for (int i = 0; i < tabela->numero_colunas; i++)
        if (contexto_igual(tabela->colunas[i].nome, nome))
            return i;
    return -1;
}

/* ------------------------------------------------------------------ */
/* Registro de tabelas (CREATE TABLE)                                  */
/* ------------------------------------------------------------------ */

/*
 * Unico ponto de entrada de novas tabelas: usado tanto pelo CREATE TABLE
 * quanto ao carregar o arquivo do catalogo, entao as mesmas regras valem
 * nos dois casos.
 */
static int catalogo_adicionar_tabela(Catalogo *cat, const char *nome,
                                     const Coluna *colunas, int n)
{
    TabelaCat nova;
    int total = 0;

    if (!nome_valido(nome))
        return erro_cat(cat, "nome de tabela invalido '%s'", nome);
    if (catalogo_buscar_tabela(cat, nome))
        return erro_cat(cat, "tabela '%s' ja existe", nome);
    if (cat->numero_tabelas >= MAX_TABELAS)
        return erro_cat(cat, "numero maximo de tabelas (%d) excedido", MAX_TABELAS);
    if (n < 1 || n > MAX_COLUNAS)
        return erro_cat(cat, "tabela '%s' deve ter de 1 a %d colunas", nome, MAX_COLUNAS);

    memset(&nova, 0, sizeof nova);
    strcpy(nova.nome, nome);
    montar_nome_arquivo(nova.arquivo, nome);

    for (int i = 0; i < n; i++) {
        ColunaCat *c = &nova.colunas[i];

        if (!nome_valido(colunas[i].nome))
            return erro_cat(cat, "nome de coluna invalido '%s'", colunas[i].nome);
        if (colunas[i].tipo != TOKEN_INT && colunas[i].tipo != TOKEN_CHAR)
            return erro_cat(cat, "coluna '%s' com tipo desconhecido", colunas[i].nome);
        for (int j = 0; j < i; j++)
            if (contexto_igual(nova.colunas[j].nome, colunas[i].nome))
                return erro_cat(cat, "coluna '%s' duplicada", colunas[i].nome);

        strcpy(c->nome, colunas[i].nome);
        c->tipo         = colunas[i].tipo;
        c->tamanho      = tamanho_do_tipo(c->tipo);
        c->deslocamento = total;
        total          += c->tamanho;
    }

    if (total > TAM_REGISTRO)
        return erro_cat(cat,
            "as colunas de '%s' ocupam %d bytes (INT=%d, CHAR=%d), mas o registro "
            "tem tamanho fixo de %d bytes",
            nome, total, TAM_INT, TAM_CHAR, TAM_REGISTRO);
    if (n > 2 || colunas[0].tipo != TOKEN_INT)
        return erro_cat(cat,
            "a integracao atual com a arvore B+ exige a primeira coluna INT e no maximo duas colunas");

    nova.numero_colunas   = n;
    nova.tamanho_util     = total;
    nova.tamanho_registro = TAM_REGISTRO;

    cat->tabelas[cat->numero_tabelas++] = nova;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Analise semantica de INSERT e SELECT                                */
/* ------------------------------------------------------------------ */

/* Confere se o valor cabe na coluna (tipo e, para CHAR, tamanho) */
static int valor_compativel(Catalogo *cat, const TabelaCat *tabela,
                            const ColunaCat *coluna, const Valor *valor,
                            const char *contexto)
{
    if (coluna->tipo == TOKEN_INT && valor->tipo != TOKEN_NUMERO)
        return erro_cat(cat, "%s: a coluna '%s' da tabela '%s' e INT, mas o valor '%s' e string",
                        contexto, coluna->nome, tabela->nome, valor->str);

    if (coluna->tipo == TOKEN_CHAR) {
        if (valor->tipo != TOKEN_STRING)
            return erro_cat(cat, "%s: a coluna '%s' da tabela '%s' e CHAR, mas o valor %d e numero",
                            contexto, coluna->nome, tabela->nome, valor->numero);
        if (strlen(valor->str) > (size_t)coluna->tamanho)
            return erro_cat(cat, "%s: '%s' tem %d caracteres, mas a coluna '%s' comporta %d",
                            contexto, valor->str, (int)strlen(valor->str),
                            coluna->nome, coluna->tamanho);
    }
    return 1;
}

static int catalogo_validar_insert(Catalogo *cat, const Comando *cmd)
{
    const TabelaCat *tabela = catalogo_buscar_tabela(cat, cmd->tabela);

    if (!tabela)
        return erro_cat(cat, "tabela '%s' nao existe", cmd->tabela);

    if (cmd->numero_valores != tabela->numero_colunas)
        return erro_cat(cat, "INSERT em '%s' espera %d valor(es), mas recebeu %d",
                        tabela->nome, tabela->numero_colunas, cmd->numero_valores);

    for (int i = 0; i < cmd->numero_valores; i++)
        if (!valor_compativel(cat, tabela, &tabela->colunas[i], &cmd->valores[i], "INSERT"))
            return 0;
    return 1;
}

static int catalogo_validar_select(Catalogo *cat, const Comando *cmd)
{
    const TabelaCat *tabela = catalogo_buscar_tabela(cat, cmd->tabela);

    if (!tabela)
        return erro_cat(cat, "tabela '%s' nao existe", cmd->tabela);

    for (int i = 0; i < cmd->numero_condicoes; i++) {
        const Condicao *cond = &cmd->condicoes[i];
        int indice = tabela_buscar_coluna(tabela, cond->coluna);

        if (indice < 0)
            return erro_cat(cat, "coluna '%s' nao existe na tabela '%s'",
                            cond->coluna, tabela->nome);
        if (!valor_compativel(cat, tabela, &tabela->colunas[indice], &cond->valor, "WHERE"))
            return 0;
    }
    return 1;
}


static int selecionar_arquivo_tabela(Catalogo *cat, const TabelaCat *tabela) {
    if (reiniciar_cache() != 0)
        return erro_cat(cat, "nao foi possivel descarregar/reiniciar a cache");
    configura_arquivo(tabela->arquivo);
    if (inicializa_arvore() != 0)
        return erro_cat(cat, "nao foi possivel inicializar a arvore B+ de '%s'", tabela->nome);
    return 1;
}

static int empacotar_char(const char *texto, int32_t *saida) {
    uint32_t valor = 0;
    size_t n = strlen(texto);
    if (n > 4) return 0;
    for (size_t i = 0; i < n; i++)
        valor |= ((uint32_t)(unsigned char)texto[i]) << (i * 8);
    *saida = (int32_t)valor;
    return 1;
}

static void desempacotar_char(int32_t valor, char saida[5]) {
    uint32_t u = (uint32_t)valor;
    for (int i = 0; i < 4; i++) saida[i] = (char)((u >> (i * 8)) & 0xFF);
    saida[4] = '\0';
    for (int i = 3; i >= 0 && saida[i] == '\0'; i--) saida[i] = '\0';
}

static int catalogo_executar_insert(Catalogo *cat, const Comando *cmd) {
    const TabelaCat *tabela = catalogo_buscar_tabela(cat, cmd->tabela);
    if (!catalogo_validar_insert(cat, cmd)) return 0;
    if (!selecionar_arquivo_tabela(cat, tabela)) return 0;
    Registro reg = {0, 0};
    reg.id = cmd->valores[0].numero;
    if (tabela->numero_colunas > 1) {
        if (tabela->colunas[1].tipo == TOKEN_INT) reg.valor = cmd->valores[1].numero;
        else if (!empacotar_char(cmd->valores[1].str, &reg.valor))
            return erro_cat(cat, "CHAR da coluna '%s' nao cabe no formato fisico de 4 bytes", tabela->colunas[1].nome);
    }
    int r = inserir_valor(&reg);
    if (r == INSERE_DUPLICADO)
        return erro_cat(cat, "chave duplicada: '%s' deve ser unica na tabela '%s'", tabela->colunas[0].nome, tabela->nome);
    if (r != INSERE_OK)
        return erro_cat(cat, "falha ao inserir registro na arvore B+ de '%s'", tabela->nome);
    if (descarregar_todo_cache() != 0 || sincroniza_dados() != 0)
        return erro_cat(cat, "falha ao persistir registro da tabela '%s'", tabela->nome);
    return 1;
}

typedef struct {
    const TabelaCat *tabela;
    const Comando *comando;
    int encontrados;
} ContextoSelect;

static int comparar_int(int32_t a, TipoToken op, int32_t b) {
    switch (op) {
        case TOKEN_IGUAL: return a == b;
        case TOKEN_DIFERENTE: return a != b;
        case TOKEN_MENOR: return a < b;
        case TOKEN_MENOR_IGUAL: return a <= b;
        case TOKEN_MAIOR: return a > b;
        case TOKEN_MAIOR_IGUAL: return a >= b;
        default: return 0;
    }
}

static int comparar_char(const char *a, TipoToken op, const char *b) {
    int c = strcmp(a, b);
    switch (op) {
        case TOKEN_IGUAL: return c == 0;
        case TOKEN_DIFERENTE: return c != 0;
        case TOKEN_MENOR: return c < 0;
        case TOKEN_MENOR_IGUAL: return c <= 0;
        case TOKEN_MAIOR: return c > 0;
        case TOKEN_MAIOR_IGUAL: return c >= 0;
        default: return 0;
    }
}

static int registro_corresponde(const ContextoSelect *ctx, const Registro *reg) {
    const TabelaCat *t = ctx->tabela;
    for (int i = 0; i < ctx->comando->numero_condicoes; i++) {
        const Condicao *cond = &ctx->comando->condicoes[i];
        int idx = tabela_buscar_coluna(t, cond->coluna);
        if (idx == 0) {
            if (!comparar_int(reg->id, cond->operacao, cond->valor.numero)) return 0;
        } else if (idx == 1 && t->colunas[1].tipo == TOKEN_INT) {
            if (!comparar_int(reg->valor, cond->operacao, cond->valor.numero)) return 0;
        } else if (idx == 1) {
            char armazenado[5];
            desempacotar_char(reg->valor, armazenado);
            if (!comparar_char(armazenado, cond->operacao, cond->valor.str)) return 0;
        }
    }
    return 1;
}

static void imprimir_registro(const TabelaCat *t, const Registro *reg) {
    printf("  ");
    for (int i = 0; i < t->numero_colunas; i++) {
        if (i) printf(" | ");
        if (i == 0) printf("%d", reg->id);
        else if (t->colunas[i].tipo == TOKEN_INT) printf("%d", reg->valor);
        else {
            char texto[5];
            desempacotar_char(reg->valor, texto);
            printf("'%s'", texto);
        }
    }
    printf("\n");
}

static int callback_select(const Registro *reg, void *arg) {
    ContextoSelect *ctx = (ContextoSelect *)arg;
    if (registro_corresponde(ctx, reg)) {
        imprimir_registro(ctx->tabela, reg);
        ctx->encontrados++;
    }
    return 0;
}

static int catalogo_executar_select(Catalogo *cat, const Comando *cmd) {
    const TabelaCat *tabela = catalogo_buscar_tabela(cat, cmd->tabela);
    if (!catalogo_validar_select(cat, cmd)) return 0;
    if (!selecionar_arquivo_tabela(cat, tabela)) return 0;
    printf("Resultado de SELECT em '%s':\n  ", tabela->nome);
    for (int i = 0; i < tabela->numero_colunas; i++)
        printf("%s%s", i ? " | " : "", tabela->colunas[i].nome);
    printf("\n");
    ContextoSelect ctx = { tabela, cmd, 0 };
    int r = percorrer_registros(callback_select, &ctx);
    if (r < 0) return erro_cat(cat, "falha ao percorrer registros da tabela '%s'", tabela->nome);
    if (ctx.encontrados == 0) printf("  (nenhum registro encontrado)\n");
    printf("  %d registro(s) encontrado(s).\n", ctx.encontrados);
    return 1;
}

/* Aplica um comando ja aceito pelo parser. 1 = ok, 0 = erro (ver mensagem) */
static int catalogo_executar(Catalogo *cat, const Comando *cmd)
{
    switch (cmd->tipo) {
        case CMD_CREATE: {
            if (!catalogo_adicionar_tabela(cat, cmd->tabela, cmd->colunas, cmd->numero_colunas)) return 0;
            const TabelaCat *t = catalogo_buscar_tabela(cat, cmd->tabela);
            if (!selecionar_arquivo_tabela(cat, t)) { cat->numero_tabelas--; return 0; }
            if (descarregar_todo_cache() != 0 || sincroniza_dados() != 0)
                return erro_cat(cat, "falha ao persistir estrutura da tabela '%s'", t->nome);
            return 1;
        }
        case CMD_INSERT:
            return catalogo_executar_insert(cat, cmd);
        case CMD_SELECT:
            return catalogo_executar_select(cat, cmd);
    }
    return erro_cat(cat, "comando desconhecido");
}

/* ------------------------------------------------------------------ */
/* Impressao                                                           */
/* ------------------------------------------------------------------ */

static void imprimir_tabela(const TabelaCat *tabela)
{
    printf("  Tabela %s  (arquivo: %s)\n", tabela->nome, tabela->arquivo);
    printf("  registro: %d bytes (%d uteis + %d de preenchimento)\n",
           tabela->tamanho_registro, tabela->tamanho_util,
           tabela->tamanho_registro - tabela->tamanho_util);
    printf("    %-3s %-20s %-6s %-6s %s\n", "#", "COLUNA", "TIPO", "BYTES", "BYTES NO REGISTRO");
    for (int i = 0; i < tabela->numero_colunas; i++) {
        const ColunaCat *c = &tabela->colunas[i];
        printf("    %-3d %-20s %-6s %-6d %d..%d\n", i + 1, c->nome, nome_token(c->tipo),
               c->tamanho, c->deslocamento, c->deslocamento + c->tamanho - 1);
    }
}

static void catalogo_imprimir(const Catalogo *cat)
{
    printf("=== CATALOGO (%d tabela(s)) ===\n", cat->numero_tabelas);
    if (cat->numero_tabelas == 0)
        printf("  (vazio)\n");
    for (int i = 0; i < cat->numero_tabelas; i++) {
        imprimir_tabela(&cat->tabelas[i]);
        if (i + 1 < cat->numero_tabelas)
            printf("\n");
    }
}

/* ------------------------------------------------------------------ */
/* Persistencia                                                        */
/*                                                                     */
/* Formato do arquivo (texto):                                         */
/*   CATALOGO 1                                                        */
/*   TABELAS 1                                                         */
/*   TABELA alunos 2                                                   */
/*   COLUNA id INT                                                     */
/*   COLUNA nome CHAR                                                  */
/* ------------------------------------------------------------------ */

static int catalogo_salvar(const Catalogo *cat, const char *caminho)
{
    char temporario[512];
    snprintf(temporario, sizeof temporario, "%s.tmp", caminho);

    FILE *f = fopen(temporario, "w");
    if (!f)
        return 0;

    fprintf(f, "%s %d\n", CAT_MAGICA, CAT_VERSAO);
    fprintf(f, "TABELAS %d\n", cat->numero_tabelas);
    for (int i = 0; i < cat->numero_tabelas; i++) {
        const TabelaCat *t = &cat->tabelas[i];
        fprintf(f, "TABELA %s %d\n", t->nome, t->numero_colunas);
        for (int j = 0; j < t->numero_colunas; j++)
            fprintf(f, "COLUNA %s %s\n", t->colunas[j].nome, nome_token(t->colunas[j].tipo));
    }

    int falhou = ferror(f);
    if (fclose(f) != 0)
        falhou = 1;
    if (falhou) {
        remove(temporario);
        return 0;
    }

    /* grava num arquivo temporario e so depois troca, para nao corromper o
     * catalogo se algo der errado no meio da escrita */
#ifdef _WIN32
    remove(caminho);   /* no Windows, rename falha se o destino ja existe */
#endif
    if (rename(temporario, caminho) != 0) {
        remove(temporario);
        return 0;
    }
    return 1;
}

/* Le uma palavra (sem espacos) que caiba em 'capacidade' bytes */
static int ler_palavra(FILE *f, char *saida, size_t capacidade)
{
    char tmp[256];
    if (fscanf(f, "%255s", tmp) != 1 || strlen(tmp) >= capacidade)
        return 0;
    strcpy(saida, tmp);
    return 1;
}

static int ler_inteiro(FILE *f, int *saida)
{
    return fscanf(f, "%d", saida) == 1;
}

static int carregar_de_arquivo(Catalogo *cat, FILE *f)
{
    char palavra[MAX_LEXEMA];
    int versao, total;

    if (!ler_palavra(f, palavra, sizeof palavra) || strcmp(palavra, CAT_MAGICA) != 0)
        return erro_cat(cat, "cabecalho invalido (nao e um arquivo de catalogo)");
    if (!ler_inteiro(f, &versao) || versao != CAT_VERSAO)
        return erro_cat(cat, "versao de catalogo nao suportada");

    if (!ler_palavra(f, palavra, sizeof palavra) || strcmp(palavra, "TABELAS") != 0 ||
        !ler_inteiro(f, &total) || total < 0 || total > MAX_TABELAS)
        return erro_cat(cat, "numero de tabelas invalido");

    for (int i = 0; i < total; i++) {
        char   nome[MAX_LEXEMA];
        Coluna colunas[MAX_COLUNAS];
        int    n;

        if (!ler_palavra(f, palavra, sizeof palavra) || strcmp(palavra, "TABELA") != 0 ||
            !ler_palavra(f, nome, sizeof nome) ||
            !ler_inteiro(f, &n) || n < 1 || n > MAX_COLUNAS)
            return erro_cat(cat, "entrada TABELA %d invalida", i + 1);

        for (int j = 0; j < n; j++) {
            char tipo[MAX_LEXEMA];

            if (!ler_palavra(f, palavra, sizeof palavra) || strcmp(palavra, "COLUNA") != 0 ||
                !ler_palavra(f, colunas[j].nome, sizeof colunas[j].nome) ||
                !ler_palavra(f, tipo, sizeof tipo))
                return erro_cat(cat, "tabela '%s': entrada COLUNA %d invalida", nome, j + 1);

            if (strcmp(tipo, "INT") == 0)        colunas[j].tipo = TOKEN_INT;
            else if (strcmp(tipo, "CHAR") == 0)  colunas[j].tipo = TOKEN_CHAR;
            else
                return erro_cat(cat, "tabela '%s': tipo '%s' desconhecido", nome, tipo);
        }

        if (!catalogo_adicionar_tabela(cat, nome, colunas, n))
            return 0;   /* a mensagem ja foi gravada */
    }
    return 1;
}

/*
 * Devolve:  1 = catalogo carregado
 *           0 = arquivo nao existe (catalogo novo e vazio)
 *          -1 = arquivo existe mas e invalido (ver cat->mensagem)
 */
static int catalogo_carregar(Catalogo *cat, const char *caminho)
{
    catalogo_iniciar(cat);

    FILE *f = fopen(caminho, "r");
    if (!f)
        return 0;

    int ok = carregar_de_arquivo(cat, f);
    fclose(f);

    if (!ok) {
        cat->numero_tabelas = 0;    /* nao deixa um catalogo pela metade */
        return -1;
    }
    return 1;
}
