#ifndef QUERY_VALIDATOR
#define QUERY_VALIDATOR

/*
 * QueryValidator.h
 *
 * Valida uma consulta (AST produzida por Parser.c) contra o catalogo
 * (Catalog.c) e devolve uma representacao validada e "pre-resolvida":
 *
 *   - tabela resolvida (nome canonico do catalogo, arquivo de dados);
 *   - colunas resolvidas para indice, tipo, tamanho, deslocamento e campo
 *     fisico do registro (ID_CAMPO / VALOR_CAMPO);
 *   - valores ja convertidos para o formato fisico (CHAR empacotado);
 *   - operadores ja checados contra o tipo da coluna.
 *
 * Fases:
 *   1. Validacao sintatica : consistencia estrutural da AST (tipos, limites,
 *                            nomes, secoes que nao pertencem ao comando).
 *   2. Validacao semantica : tabela, colunas, quantidade de valores, tipos,
 *                            tamanho de CHAR, operadores e regras do CREATE.
 *
 * Garantias:
 *   - O validador NAO le nem grava registros e NAO altera o catalogo: tudo e
 *     decidido antes de qualquer acesso a arvore B+. Se validar_consulta
 *     devolver 0, a consulta deve ser descartada.
 *   - ConsultaValidada e autocontida (sem ponteiros para o catalogo ou para a
 *     AST) e nao guarda nomes de coluna que precisem ser buscados de novo.
 *     Os nomes presentes servem apenas para exibir cabecalhos.
 *   - Todos os erros encontrados em uma fase sao acumulados no relatorio.
 *
 * Dependencias: incluir DEPOIS de Parser.c e Catalog.c (mesmo padrao do
 * projeto, que inclui os .c diretamente).
 *
 *   Uso:
 *       Comando cmd;  ConsultaValidada q;  QVRelatorio rel;
 *       if (parse_comando(&parser, &cmd) &&
 *           validar_consulta(&catalogo, &cmd, &q, &rel)) {
 *           // executar usando apenas 'q'
 *       } else {
 *           imprimir_relatorio(&rel, stderr);
 *       }
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ERR_QV 32        /* erros guardados no relatorio (resto e contado) */
#define MAX_MENSAGEM 64

/* ------------------------------------------------------------------ */
/* Relatorio de erros                                                  */
/* ------------------------------------------------------------------ */

typedef enum {
    FASE_SINTATICA,
    FASE_SEMANTICA
} FaseQV;

typedef enum {
    /* sintaticos */
    ERRO_ARGUMENTO_NULO,
    ERRO_COMANDO_DESCONHECIDO,
    ERRO_NOME_AUSENTE,
    ERRO_LIMITE_EXCEDIDO,
    ERRO_ESTRUTURA_INCONSISTENTE,
    ERRO_TIPO_COLUNA_INVALIDO,
    ERRO_TIPO_VALOR_INVALIDO,
    ERRO_OPERADOR_INVALIDO,
    /* semanticos */
    ERRO_TABELA_INEXISTENTE,
    ERRO_TABELA_JA_EXISTE,
    ERRO_DEFINICAO_TABELA,
    ERRO_LAYOUT_FISICO,
    ERRO_COLUNA_INEXISTENTE,
    ERRO_QUANTIDADE_VALORES,
    ERRO_TIPO_INCOMPATIVEL,
    ERRO_CHAR_EXCEDE_TAMANHO,
    ERRO_OPERADOR_INCOMPATIVEL,
    ERRO_MEMORIA
} CodigosQV;

typedef struct {
    FaseQV   fase;
    CodigosQV codigo;
    int      posicao;                   /* indice (0-based) da coluna, valor ou
                                          condicao; -1 se nao se aplica   */
    char  mensagem[MAX_MENSAGEM];
}ErroQV;

typedef struct {
    ErroQV erros[MAX_ERR_QV];
    int    total;                       /* erros guardados                  */
    int    descartados;                 /* erros alem de MAX_ERR_QV       */
} QVRelatorio;

static void iniciar_relatorio(QVRelatorio *rel)
{
    memset(rel, 0, sizeof *rel);
}

static int total_erros(const QVRelatorio *rel)
{
    return rel->total + rel->descartados;
}

static int relatorio_possui_erros(const QVRelatorio *rel)
{
    return total_erros(rel) > 0;
}

static void registrar(QVRelatorio *rel, FaseQV fase, CodigosQV codigo,
                         int posicao, const char *formato, ...)
{
    ErroQV *erro;
    va_list ap;

    if (rel->total >= MAX_ERR_QV) {
        rel->descartados++;
        return;
    }

    erro = &rel->erros[rel->total++];
    erro->fase    = fase;
    erro->codigo  = codigo;
    erro->posicao = posicao;

    va_start(ap, formato);
    vsnprintf(erro->mensagem, MAX_MENSAGEM, formato, ap);
    va_end(ap);
}

static const char *nome_fase(FaseQV fase)
{
    return fase == FASE_SINTATICA ? "SINTATICA" : "SEMANTICA";
}

static const char *nome_codigo(CodigosQV codigo)
{
    switch (codigo) {
        case ERRO_ARGUMENTO_NULO:           return "ARGUMENTO_NULO";
        case ERRO_COMANDO_DESCONHECIDO:     return "COMANDO_DESCONHECIDO";
        case ERRO_NOME_AUSENTE:             return "NOME_AUSENTE";
        case ERRO_LIMITE_EXCEDIDO:          return "LIMITE_EXCEDIDO";
        case ERRO_ESTRUTURA_INCONSISTENTE:  return "ESTRUTURA_INCONSISTENTE";
        case ERRO_TIPO_COLUNA_INVALIDO:     return "TIPO_COLUNA_INVALIDO";
        case ERRO_TIPO_VALOR_INVALIDO:      return "TIPO_VALOR_INVALIDO";
        case ERRO_OPERADOR_INVALIDO:        return "OPERADOR_INVALIDO";
        case ERRO_TABELA_INEXISTENTE:       return "TABELA_INEXISTENTE";
        case ERRO_TABELA_JA_EXISTE:         return "TABELA_JA_EXISTE";
        case ERRO_DEFINICAO_TABELA:         return "DEFINICAO_TABELA";
        case ERRO_LAYOUT_FISICO:            return "LAYOUT_FISICO";
        case ERRO_COLUNA_INEXISTENTE:       return "COLUNA_INEXISTENTE";
        case ERRO_QUANTIDADE_VALORES:       return "QUANTIDADE_VALORES";
        case ERRO_TIPO_INCOMPATIVEL:        return "TIPO_INCOMPATIVEL";
        case ERRO_CHAR_EXCEDE_TAMANHO:      return "CHAR_EXCEDE_TAMANHO";
        case ERRO_OPERADOR_INCOMPATIVEL:    return "OPERADOR_INCOMPATIVEL";
        case ERRO_MEMORIA:                  return "MEMORIA";
    }
    return "DESCONHECIDO";
}

/* Imprime o relatorio completo (ou "Consulta valida." se nao houver erros) */
static void imprimir_relatorio(const QVRelatorio *rel, FILE *saida)
{
    if (!relatorio_possui_erros(rel)) {
        fprintf(saida, "Consulta valida.\n");
        return;
    }

    fprintf(saida, "Consulta rejeitada: %d erro(s)\n", total_erros(rel));
    for (int i = 0; i < rel->total; i++)
        fprintf(saida, "  %2d. [%s/%s] %s\n", i + 1,
                nome_fase(rel->erros[i].fase),
                nome_codigo(rel->erros[i].codigo),
                rel->erros[i].mensagem);
    if (rel->descartados > 0)
        fprintf(saida, "  ... e mais %d erro(s) omitido(s)\n", rel->descartados);
}

/* ------------------------------------------------------------------ */
/* Representacao validada                                              */
/* ------------------------------------------------------------------ */

/* Campo fisico do Registro que armazena a coluna (coluna 0 -> id, 1 -> valor) */
typedef enum {
    ID_CAMPO,
    VALOR_CAMPO
} CampoQV;

typedef struct {
    int       indice;                   /* posicao na tabela do catalogo    */
    char      nome[MAX_LEXEMA];         /* nome canonico (so para exibicao) */
    TipoToken tipo;                     /* TOKEN_INT ou TOKEN_CHAR          */
    int       tamanho;
    int       deslocamento;
    CampoQV   campo;
} ColunaQV;

/* Valor ja no formato do campo fisico */
typedef struct {
    TipoToken tipo;                     /* tipo da COLUNA (INT ou CHAR)     */
    int32_t   inteiro;                  /* INT: o valor; CHAR: 4 bytes empacotados */
    char      texto[TAM_CHAR + 1];      /* CHAR: texto original (comparacao lexicografica) */
} OperandoQV;

typedef struct {                        /* INSERT: valor -> coluna          */
    int        indice_coluna;
    CampoQV    campo;
    OperandoQV operando;
} AtribuicaoQV;

typedef struct {                        /* WHERE: coluna operador valor     */
    int        indice_coluna;
    CampoQV    campo;
    TipoToken  operador;                /* TOKEN_IGUAL, TOKEN_MENOR, ...    */
    OperandoQV operando;
} CondicaoQV;

typedef struct {
    int         valida;                 /* 1 somente se a validacao passou  */
    TipoComando tipo;

    char        nome_tabela[MAX_LEXEMA];/* nome canonico do catalogo        */
    char        arquivo[TAM_ARQUIVO];   /* arquivo de dados (nome.dat)      */
    int         indice_tabela;          /* posicao no catalogo; -1 no CREATE*/
    int         tamanho_util;
    int         tamanho_registro;

    /* CREATE: colunas a criar. INSERT/SELECT: colunas da tabela (projecao
     * de SELECT *), na ordem de exibicao. */
    ColunaQV    colunas[MAX_COLUNAS];
    int         numero_colunas;

    AtribuicaoQV atribuicoes[MAX_VALORES];      /* INSERT: uma por coluna   */
    int          numero_atribuicoes;

    CondicaoQV   condicoes[MAX_CONDICOES];      /* SELECT ... WHERE         */
    int          numero_condicoes;
} ConsultaValidada;

/* ------------------------------------------------------------------ */
/* Auxiliares                                                          */
/* ------------------------------------------------------------------ */

static int texto_terminou(const char *texto, size_t capacidade)
{
    return memchr(texto, '\0', capacidade) != NULL;
}

static int texto_valido(const char *texto, size_t capacidade)
{
    return texto_terminou(texto, capacidade) && texto[0] != '\0';
}

static const char *tipo_nome(TipoToken tipo)
{
    if (tipo == TOKEN_INT)  return "INT";
    if (tipo == TOKEN_CHAR) return "CHAR";
    return "?";
}

static CampoQV campo_da_coluna(int indice)
{
    return indice == 0 ? ID_CAMPO : VALOR_CAMPO;
}

static int tipo_coluna_valido(TipoToken tipo)
{
    return tipo == TOKEN_INT || tipo == TOKEN_CHAR;
}

static int tipo_valor_valido(TipoToken tipo)
{
    return tipo == TOKEN_NUMERO || tipo == TOKEN_STRING;
}

/* Regra unica de compatibilidade operador x tipo de coluna. INT usa
 * comparacao numerica; CHAR, comparacao lexicografica. */
static int operador_suportado(TipoToken tipo_coluna, TipoToken operador)
{
    switch (tipo_coluna) {
        case TOKEN_INT:
        case TOKEN_CHAR:
            return e_operador(operador);
        default:
            return 0;
    }
}

/* Confere 0 <= n <= maximo; registra erro e devolve 0 se estiver fora */
static int contagem_ok(QVRelatorio *rel, const char *secao, int n, int maximo)
{
    if (n >= 0 && n <= maximo)
        return 1;
    registrar(rel, FASE_SINTATICA, ERRO_LIMITE_EXCEDIDO, -1,
                 "quantidade de %s invalida (%d); o maximo e %d", secao, n, maximo);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Fase 1: validacao sintatica da AST                                  */
/* ------------------------------------------------------------------ */

static void sintaxe_secoes_alheias(const Comando *cmd, QVRelatorio *rel)
{
    if (cmd->tipo != CMD_CREATE && cmd->numero_colunas != 0)
        registrar(rel, FASE_SINTATICA, ERRO_ESTRUTURA_INCONSISTENTE, -1,
                     "somente CREATE TABLE pode ter definicao de colunas");
    if (cmd->tipo != CMD_INSERT && cmd->numero_valores != 0)
        registrar(rel, FASE_SINTATICA, ERRO_ESTRUTURA_INCONSISTENTE, -1,
                     "somente INSERT pode ter lista de valores");
    if (cmd->tipo != CMD_SELECT && cmd->numero_condicoes != 0)
        registrar(rel, FASE_SINTATICA, ERRO_ESTRUTURA_INCONSISTENTE, -1,
                     "somente SELECT pode ter condicoes WHERE");
}

static void sintaxe_valor(const Valor *valor, const char *contexto, int posicao,
                             QVRelatorio *rel)
{
    if (!tipo_valor_valido(valor->tipo)) {
        registrar(rel, FASE_SINTATICA, ERRO_TIPO_VALOR_INVALIDO, posicao,
                     "%s %d: valor deve ser numero ou string", contexto, posicao + 1);
        return;
    }
    if (valor->tipo == TOKEN_STRING && !texto_terminou(valor->str, MAX_LEXEMA))
        registrar(rel, FASE_SINTATICA, ERRO_TIPO_VALOR_INVALIDO, posicao,
                     "%s %d: string sem terminador", contexto, posicao + 1);
}

static void sintaxe_create(const Comando *cmd, QVRelatorio *rel)
{
    if (!contagem_ok(rel, "colunas", cmd->numero_colunas, MAX_COLUNAS))
        return;
    if (cmd->numero_colunas == 0)
        registrar(rel, FASE_SINTATICA, ERRO_ESTRUTURA_INCONSISTENTE, -1,
                     "CREATE TABLE exige ao menos uma coluna");

    for (int i = 0; i < cmd->numero_colunas; i++) {
        const Coluna *coluna = &cmd->colunas[i];

        if (!texto_valido(coluna->nome, MAX_LEXEMA))
            registrar(rel, FASE_SINTATICA, ERRO_NOME_AUSENTE, i,
                         "coluna %d sem nome valido", i + 1);
        if (!tipo_coluna_valido(coluna->tipo))
            registrar(rel, FASE_SINTATICA, ERRO_TIPO_COLUNA_INVALIDO, i,
                         "coluna %d: tipo deve ser INT ou CHAR", i + 1);
    }
}

static void sintaxe_insert(const Comando *cmd, QVRelatorio *rel)
{
    if (!contagem_ok(rel, "valores", cmd->numero_valores, MAX_VALORES))
        return;
    if (cmd->numero_valores == 0)
        registrar(rel, FASE_SINTATICA, ERRO_ESTRUTURA_INCONSISTENTE, -1,
                     "INSERT exige ao menos um valor");

    for (int i = 0; i < cmd->numero_valores; i++)
        sintaxe_valor(&cmd->valores[i], "valor", i, rel);
}

static void sintaxe_select(const Comando *cmd, QVRelatorio *rel)
{
    if (!contagem_ok(rel, "condicoes", cmd->numero_condicoes, MAX_CONDICOES))
        return;

    for (int i = 0; i < cmd->numero_condicoes; i++) {
        const Condicao *cond = &cmd->condicoes[i];

        if (!texto_valido(cond->coluna, MAX_LEXEMA))
            registrar(rel, FASE_SINTATICA, ERRO_NOME_AUSENTE, i,
                         "condicao %d sem nome de coluna valido", i + 1);
        if (!e_operador(cond->operacao))
            registrar(rel, FASE_SINTATICA, ERRO_OPERADOR_INVALIDO, i,
                         "condicao %d: operador de comparacao invalido", i + 1);
        sintaxe_valor(&cond->valor, "condicao", i, rel);
    }
}

/* Devolve 1 se a AST esta estruturalmente consistente */
static int validar_sintaxe(const Comando *cmd, QVRelatorio *rel)
{
    int antes = total_erros(rel);

    switch (cmd->tipo) {
        case CMD_CREATE:
        case CMD_INSERT:
        case CMD_SELECT:
            break;
        default:
            registrar(rel, FASE_SINTATICA, ERRO_COMANDO_DESCONHECIDO, -1,
                         "tipo de comando %d desconhecido", (int)cmd->tipo);
            return 0;
    }

    if (!texto_valido(cmd->tabela, MAX_LEXEMA))
        registrar(rel, FASE_SINTATICA, ERRO_NOME_AUSENTE, -1,
                     "nome da tabela ausente ou invalido");

    sintaxe_secoes_alheias(cmd, rel);

    switch (cmd->tipo) {
        case CMD_CREATE: sintaxe_create(cmd, rel); break;
        case CMD_INSERT: sintaxe_insert(cmd, rel); break;
        case CMD_SELECT: sintaxe_select(cmd, rel); break;
    }

    return total_erros(rel) == antes;
}

/* ------------------------------------------------------------------ */
/* Fase 2: validacao semantica                                         */
/* ------------------------------------------------------------------ */

/* Copia os metadados da tabela para a consulta (sem ponteiros para o catalogo) */
static void copiar_tabela(ConsultaValidada *q, const TabelaCat *tabela, int indice)
{
    strcpy(q->nome_tabela, tabela->nome);
    strcpy(q->arquivo, tabela->arquivo);
    q->indice_tabela    = indice;
    q->tamanho_util     = tabela->tamanho_util;
    q->tamanho_registro = tabela->tamanho_registro;
    q->numero_colunas   = tabela->numero_colunas;

    for (int i = 0; i < tabela->numero_colunas; i++) {
        ColunaQV *destino = &q->colunas[i];
        const ColunaCat *origem = &tabela->colunas[i];

        destino->indice       = i;
        strcpy(destino->nome, origem->nome);
        destino->tipo         = origem->tipo;
        destino->tamanho      = origem->tamanho;
        destino->deslocamento = origem->deslocamento;
        destino->campo        = campo_da_coluna(i);
    }
}

/* Resolve a tabela do comando e confere se o layout fisico a comporta
 * (coluna 0 INT -> id; no maximo uma segunda coluna -> valor). */
static const TabelaCat *resolver_tabela(const Catalogo *cat, const Comando *cmd,
                                           QVRelatorio *rel)
{
    const TabelaCat *tabela = catalogo_buscar_tabela(cat, cmd->tabela);

    if (!tabela) {
        registrar(rel, FASE_SEMANTICA, ERRO_TABELA_INEXISTENTE, -1,
                     "tabela '%s' nao existe", cmd->tabela);
        return NULL;
    }
    if (tabela->numero_colunas < 1 || tabela->numero_colunas > 2 ||
        tabela->colunas[0].tipo != TOKEN_INT) {
        registrar(rel, FASE_SEMANTICA, ERRO_LAYOUT_FISICO, -1,
                     "tabela '%s' nao e compativel com o formato fisico do registro "
                     "(primeira coluna INT e no maximo duas colunas)", tabela->nome);
        return NULL;
    }
    return tabela;
}

/* Confere tipo/tamanho do valor para a coluna e o converte ao formato fisico */
static int resolver_valor(QVRelatorio *rel, const TabelaCat *tabela,
                             const ColunaCat *coluna, const Valor *valor,
                             const char *contexto, int posicao, OperandoQV *saida)
{
    memset(saida, 0, sizeof *saida);
    saida->tipo = coluna->tipo;

    if (coluna->tipo == TOKEN_INT) {
        if (valor->tipo != TOKEN_NUMERO) {
            registrar(rel, FASE_SEMANTICA, ERRO_TIPO_INCOMPATIVEL, posicao,
                         "%s: a coluna '%s' da tabela '%s' e INT, mas o valor '%s' e string",
                         contexto, coluna->nome, tabela->nome, valor->str);
            return 0;
        }
        saida->inteiro = (int32_t)valor->numero;
        return 1;
    }

    /* CHAR */
    if (valor->tipo != TOKEN_STRING) {
        registrar(rel, FASE_SEMANTICA, ERRO_TIPO_INCOMPATIVEL, posicao,
                     "%s: a coluna '%s' da tabela '%s' e CHAR, mas o valor %d e numero",
                     contexto, coluna->nome, tabela->nome, valor->numero);
        return 0;
    }
    if (strlen(valor->str) > (size_t)coluna->tamanho) {
        registrar(rel, FASE_SEMANTICA, ERRO_CHAR_EXCEDE_TAMANHO, posicao,
                     "%s: '%s' tem %d caracteres, mas a coluna '%s' comporta %d",
                     contexto, valor->str, (int)strlen(valor->str),
                     coluna->nome, coluna->tamanho);
        return 0;
    }
    if (!empacotar_char(valor->str, &saida->inteiro)) {
        registrar(rel, FASE_SEMANTICA, ERRO_CHAR_EXCEDE_TAMANHO, posicao,
                     "%s: '%s' nao cabe no formato fisico de %d bytes da coluna '%s'",
                     contexto, valor->str, TAM_CHAR, coluna->nome);
        return 0;
    }
    strcpy(saida->texto, valor->str);   /* empacotar_char garante len <= TAM_CHAR */
    return 1;
}

static int semantica_create(const Catalogo *cat, const Comando *cmd,
                               ConsultaValidada *saida, QVRelatorio *rel)
{
    Catalogo *copia;
    int ok;

    if (catalogo_buscar_tabela(cat, cmd->tabela)) {
        registrar(rel, FASE_SEMANTICA, ERRO_TABELA_JA_EXISTE, -1,
                     "tabela '%s' ja existe", cmd->tabela);
        return 0;
    }

    /* As regras de definicao de tabela vivem em catalogo_adicionar_tabela.
     * Ela e executada numa copia descartavel, para nao alterar o catalogo
     * real antes da execucao e sem duplicar as regras aqui. */
    copia = (Catalogo *)malloc(sizeof *copia);
    if (!copia) {
        registrar(rel, FASE_SEMANTICA, ERRO_MEMORIA, -1,
                     "memoria insuficiente para validar CREATE TABLE");
        return 0;
    }
    memcpy(copia, cat, sizeof *copia);

    ok = catalogo_adicionar_tabela(copia, cmd->tabela, cmd->colunas, cmd->numero_colunas);
    if (!ok) {
        registrar(rel, FASE_SEMANTICA, ERRO_DEFINICAO_TABELA, -1,
                     "CREATE TABLE: %s", copia->mensagem);
    } else {
        copiar_tabela(saida, &copia->tabelas[copia->numero_tabelas - 1], -1);
    }

    free(copia);
    return ok;
}

static int semantica_insert(const Catalogo *cat, const Comando *cmd,
                               ConsultaValidada *saida, QVRelatorio *rel)
{
    const TabelaCat *tabela = resolver_tabela(cat, cmd, rel);
    int antes = total_erros(rel);
    int comuns;

    if (!tabela)
        return 0;

    if (cmd->numero_valores != tabela->numero_colunas)
        registrar(rel, FASE_SEMANTICA, ERRO_QUANTIDADE_VALORES, -1,
                     "INSERT em '%s' espera %d valor(es), mas recebeu %d",
                     tabela->nome, tabela->numero_colunas, cmd->numero_valores);

    /* Mesmo com quantidade errada, valida os valores que tem coluna
     * correspondente, para relatar o maximo de problemas de uma vez. */
    comuns = cmd->numero_valores < tabela->numero_colunas
           ? cmd->numero_valores : tabela->numero_colunas;

    for (int i = 0; i < comuns; i++) {
        AtribuicaoQV *atribuicao = &saida->atribuicoes[i];

        if (!resolver_valor(rel, tabela, &tabela->colunas[i], &cmd->valores[i],
                               "INSERT", i, &atribuicao->operando))
            continue;
        atribuicao->indice_coluna = i;
        atribuicao->campo         = campo_da_coluna(i);
    }

    if (total_erros(rel) != antes)
        return 0;

    saida->numero_atribuicoes = tabela->numero_colunas;
    copiar_tabela(saida, tabela, (int)(tabela - cat->tabelas));
    return 1;
}

static int semantica_select(const Catalogo *cat, const Comando *cmd,
                               ConsultaValidada *saida, QVRelatorio *rel)
{
    const TabelaCat *tabela = resolver_tabela(cat, cmd, rel);
    int antes = total_erros(rel);

    if (!tabela)
        return 0;

    for (int i = 0; i < cmd->numero_condicoes; i++) {
        const Condicao *cond = &cmd->condicoes[i];
        CondicaoQV *resolvida = &saida->condicoes[i];
        int indice = tabela_buscar_coluna(tabela, cond->coluna);
        const ColunaCat *coluna;
        int operador_ok;
        int valor_ok;

        if (indice < 0) {
            registrar(rel, FASE_SEMANTICA, ERRO_COLUNA_INEXISTENTE, i,
                         "WHERE: coluna '%s' nao existe na tabela '%s'",
                         cond->coluna, tabela->nome);
            continue;
        }

        coluna      = &tabela->colunas[indice];
        operador_ok = operador_suportado(coluna->tipo, cond->operacao);
        if (!operador_ok)
            registrar(rel, FASE_SEMANTICA, ERRO_OPERADOR_INCOMPATIVEL, i,
                         "WHERE: operador '%s' nao se aplica a coluna '%s' (%s)",
                         simbolo_operacao(cond->operacao), coluna->nome,
                         tipo_nome(coluna->tipo));

        valor_ok = resolver_valor(rel, tabela, coluna, &cond->valor, "WHERE", i,
                                     &resolvida->operando);

        if (operador_ok && valor_ok) {
            resolvida->indice_coluna = indice;
            resolvida->campo         = campo_da_coluna(indice);
            resolvida->operador      = cond->operacao;
        }
    }

    if (total_erros(rel) != antes)
        return 0;

    saida->numero_condicoes = cmd->numero_condicoes;
    copiar_tabela(saida, tabela, (int)(tabela - cat->tabelas));
    return 1;
}

/* ------------------------------------------------------------------ */
/* Ponto de entrada                                                    */
/* ------------------------------------------------------------------ */

/*
 * Valida 'cmd' contra 'cat'. Devolve 1 e preenche 'saida' (valida = 1) se a
 * consulta e aceita; devolve 0 e deixa 'saida' zerada (valida = 0) caso
 * contrario, com os motivos em 'rel'. 'rel' pode ser NULL se so o veredito
 * interessar. Nao acessa registros e nao modifica 'cat'.
 */
static int validar_consulta(const Catalogo *cat, const Comando *cmd,
                               ConsultaValidada *saida, QVRelatorio *rel)
{
    QVRelatorio descartavel;
    int ok = 0;

    if (!rel)
        rel = &descartavel;
    iniciar_relatorio(rel);

    if (!cat || !cmd || !saida) {
        registrar(rel, FASE_SINTATICA, ERRO_ARGUMENTO_NULO, -1,
                     "catalogo, comando e saida sao obrigatorios");
        return 0;
    }
    memset(saida, 0, sizeof *saida);

    if (!validar_sintaxe(cmd, rel))
        return 0;

    switch (cmd->tipo) {
        case CMD_CREATE: ok = semantica_create(cat, cmd, saida, rel); break;
        case CMD_INSERT: ok = semantica_insert(cat, cmd, saida, rel); break;
        case CMD_SELECT: ok = semantica_select(cat, cmd, saida, rel); break;
    }

    if (!ok || relatorio_possui_erros(rel)) {
        memset(saida, 0, sizeof *saida);
        return 0;
    }

    saida->valida = 1;
    saida->tipo   = cmd->tipo;
    return 1;
}

#endif /* QUERY_VALIDATOR */
