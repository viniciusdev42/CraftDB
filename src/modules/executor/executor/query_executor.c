/* ============================================================
 *                 QUERY EXECUTOR (EXECUTOR DE CONSULTAS)
 * ============================================================
 *
 * Executa um SELECT ja analisado pelo parser e devolve um query_result.
 *
 * Pipeline (um passo por modulo, cada um com UMA responsabilidade):
 *
 *   Comando (AST do parser)
 *      |
 *      |  1) validar_consulta()          query_validator.c
 *      |       tabela, colunas, operadores e tipos dos literais conferidos
 *      |       contra o catalogo; nada e' lido do disco se a consulta for invalida
 *      v
 *   ConsultaValidada
 *      |
 *      |  2) qr_iniciar()                query_result.c
 *      |       cria o resultado com os metadados das colunas projetadas
 *      |  3) table_scan_abrir()          table_scan
.c
 *      |       abre a varredura sequencial da tabela
 *      v
 *   +--> 4) table_scan_next()            table_scan.c  -> row (tupla logica)
 *   |       |
 *   |       |  5) linha_permanece_where()  predicate_evaluator.c
 *   |       |       avalia o WHERE; erro de avaliacao aborta a consulta
 *   |       |
 *   |       +-- resultado != TRUE -> 6) descarta a linha, volta ao passo 4
 *   |       |
 *   |       |  7) qr_adicionar_row()       query_result.c
 *   |       |       aplica a projecao de colunas (ordem pedida) e acrescenta
 *   |       |       a linha projetada ao resultado
 *   +-------+
 *      |
 *      |  8) table_scan_fechar()         (sempre, inclusive em caso de erro)
 *      v
 *   query_result
 *
 * Projecao: a gramatica do parser so aceita SELECT '*', entao a AST nao carrega
 * lista de colunas. A projecao e' um parametro do executor (nomes + quantidade);
 * NULL / 0 equivale a SELECT * (todas as colunas, na ordem do catalogo). Os
 * nomes sao validados contra o catalogo ANTES da varredura.
 *
 * Garantias:
 *   - O executor nunca altera o catalogo e so le dados (varredura somente leitura).
 *   - A varredura e' sempre fechada: nenhum pin de cache fica pendente.
 *   - Em caso de erro, *resultado volta zerado (nenhuma memoria alocada).
 *
 * Convencao de retorno (igual a de qr_executar):
 *   executar_consulta -> numero de linhas do resultado (>= 0), ou -1 em erro.
 *   A mensagem do erro fica em Executor.mensagem.
 *
 * ------------------------------------------------------------
 * ATENCAO - blocos main dos modulos incluidos
 * ------------------------------------------------------------
 * expression_evaluator.c, predicate_evaluator.c e query_result.c definem um main()
 * sem protecao, e o proprio '#define main ...' / '#undef main' interno deles
 * impede que este arquivo renomeie esses mains de fora. Para incluir os tres
 * neste arquivo, proteja cada main com a macro abaixo (uma linha antes de
 * "int main(void)" e um "#endif" logo apos a "}" que o fecha):
 *
 *     expression_evaluator.c  ->  #ifdef EXPRESSION_EVALUATOR_MAIN
 *     predicate_evaluator.c   ->  #ifdef PREDICATE_EVALUATOR_MAIN
 *     query_result.c          ->  #ifdef QUERY_RESULT_MAIN
 *
 * (o cabecalho do expression_evaluator.c ja menciona -DEXPRESSION_EVALUATOR_MAIN).
 * query_validator.c e table_scan.c ja tem essa protecao (MAIN_VALIDATOR e
 * TABLE_SCAN_MAIN) e sao tratados logo abaixo.
 *
 * Compilacao:   gcc -Wall -Wextra -o executor query_executor.c
 * Para usar este arquivo como biblioteca (sem o main do final):
 *               -DQUERY_EXECUTOR_SEM_MAIN
 */

#ifndef QUERY_EXECUTOR
#define QUERY_EXECUTOR

/* Define as macros ANTES do include: o bloco main do validador e o do scan
   ficam desligados. */
#define MAIN_VALIDATOR
#define TABLE_SCAN_MAIN

#include "../validator/query_validator.c"      /* catalog.c + parser.c + validar_consulta()  */
#include "../scan/table_scan.c"                /* row.c (sem main) + table_scan               */
#include "../evaluator/predicate_evaluator.c"  /* expression_evaluator.c + linha_permanece_*  */
#include "../result/query_result.c"         /* query_result + qr_adicionar_row()           */

/* ------------------------------------------------------------------ */
/* Estado do executor                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    char mensagem[MAX_MESSAGEM];    /* motivo do ultimo erro                    */

    /* Estatisticas da ULTIMA execucao (zeradas a cada chamada). */
    int  linhas_varridas;           /* tuplas entregues pelo table scan         */
    int  linhas_descartadas;        /* tuplas cujo WHERE nao foi TRUE           */
} Executor;

static void iniciar_executor(Executor *ex)
{
    memset(ex, 0, sizeof *ex);
}

/* Grava a mensagem e devolve sempre -1, permitindo "return ex_erro(...)". */
static int ex_erro(Executor *ex, const char *formato, ...)
{
    va_list ap;
    va_start(ap, formato);
    vsnprintf(ex->mensagem, sizeof ex->mensagem, formato, ap);
    va_end(ap);
    return -1;
}

/* ------------------------------------------------------------------ */
/* Etapa 1: validacao                                                  */
/* ------------------------------------------------------------------ */

/* Confere a projecao (lista de nomes) contra a tabela ja resolvida pelo
   validador. NULL / 0 = SELECT *, sempre valido. Retorna 0 ou -1. */
static int executor_validar_projecao(Executor *ex, const TabelaCat *tabela,
                                     const char *const *nomes, int n)
{
    if (nomes == NULL || n == 0)
        return 0;

    if (n < 0 || n > MAX_COLUNAS)
        return ex_erro(ex, "projecao com %d coluna(s); o maximo e %d", n, MAX_COLUNAS);

    for (int i = 0; i < n; i++) {
        if (nomes[i] == NULL)
            return ex_erro(ex, "projecao: nome de coluna NULL (posicao %d)", i);
        if (tabela_buscar_coluna(tabela, nomes[i]) < 0)
            return ex_erro(ex, "projecao: coluna '%s' nao existe na tabela '%s'",
                           nomes[i], tabela->nome);
    }
    return 0;
}

/* Valida a AST contra o catalogo e a projecao contra a tabela. Retorna 0 ou -1. */
static int executor_validar(Executor *ex, Catalogo *cat, const Comando *comando,
                            const char *const *projecao, int n_projecao,
                            ConsultaValidada *validada)
{
    Validador validador;

    if (!validar_consulta(&validador, cat, comando, validada))
        return ex_erro(ex, "consulta invalida: %s", validador.mensagem);

    return executor_validar_projecao(ex, validada->tabela, projecao, n_projecao);
}

/* ------------------------------------------------------------------ */
/* Etapas 4 a 7: tupla -> WHERE -> projecao -> resultado               */
/* ------------------------------------------------------------------ */

/*
 Processa UMA tupla: avalia o WHERE e, somente se o resultado for TRUE, projeta
 as colunas e acrescenta a linha ao resultado.
 Retorna 0 em sucesso (a linha entrou ou foi descartada) ou -1 em erro.
*/
static int executor_processar_linha(Executor *ex, const Comando *comando, const Row *row,
                                    Avaliador *av, QueryResult *resultado)
{
    int permanece = 0;

    ex->linhas_varridas++;

    /* WHERE: erro de avaliacao nunca vira "falso" em silencio */
    if (linha_permanece_where(av, comando, row, &permanece) != 0)
        return ex_erro(ex, "WHERE: %s", av->mensagem);

    if (!permanece) {                       /* resultado != TRUE: descarta */
        ex->linhas_descartadas++;
        return 0;
    }

    /* qr_adicionar_row() aplica a projecao (colunas e ordem dos metadados) e
       grava a linha projetada. Mensagem detalhada vai para stderr. */
    if (qr_adicionar_row(resultado, row) != 0)
        return ex_erro(ex, "falha ao adicionar a linha projetada ao resultado de '%s'",
                       resultado->tabela);
    return 0;
}

/*
 Abre o table scan, entrega cada tupla a executor_processar_linha() e fecha a
 varredura em TODOS os caminhos (fim normal, erro de WHERE, erro de memoria).
 Retorna 0 ou -1.
*/
static int executor_varrer(Executor *ex, Catalogo *cat, const Comando *comando,
                           const TabelaCat *tabela, QueryResult *resultado)
{
    TableScan scan;
    Avaliador av;
    Row       row;
    int       r;
    int       status = 0;

    iniciar_avaliador(&av);

    if (table_scan_abrir(&scan, cat, tabela->nome) != 0)
        return ex_erro(ex, "nao foi possivel abrir a varredura da tabela '%s'", tabela->nome);

    while ((r = table_scan_next(&scan, &row)) == 1) {
        status = executor_processar_linha(ex, comando, &row, &av, resultado);
        if (status != 0)
            break;
    }

    if (status == 0 && r < 0)
        status = ex_erro(ex, "falha durante a varredura da tabela '%s'", tabela->nome);

    if (table_scan_fechar(&scan) != 0 && status == 0)
        status = ex_erro(ex, "falha ao fechar a varredura da tabela '%s'", tabela->nome);

    return status;
}

/* ------------------------------------------------------------------ */
/* Interface publica                                                   */
/* ------------------------------------------------------------------ */

/*
 executar_consulta(ex, cat, comando, projecao, n_projecao, resultado)

 Executa o SELECT 'comando' sobre o catalogo 'cat'.
   projecao / n_projecao : colunas a devolver, NESSA ORDEM (nomes sem diferenciar
                           maiusculas/minusculas). NULL ou 0 = todas (SELECT *).
   resultado             : recebe o query_result; o chamador deve chama-lo
                           qr_liberar() depois de consumi-lo.

 O catalogo deve continuar existindo (e nao ser movido) durante a chamada.
 Retorna o numero de linhas do resultado (>= 0), ou -1 em caso de erro, com a
 mensagem em ex->mensagem e *resultado zerado.
*/
static int executar_consulta(Executor *ex, Catalogo *cat, const Comando *comando,
                             const char *const *projecao, int n_projecao,
                             QueryResult *resultado)
{
    ConsultaValidada validada;

    if (ex == NULL)
        return -1;
    iniciar_executor(ex);

    if (cat == NULL || comando == NULL || resultado == NULL)
        return ex_erro(ex, "executar_consulta recebeu argumento NULL");

    memset(resultado, 0, sizeof *resultado);    /* estado limpo em qualquer falha */

    if (comando->tipo != CMD_SELECT)
        return ex_erro(ex, "o executor so processa SELECT");

    /* 1) validar */
    if (executor_validar(ex, cat, comando, projecao, n_projecao, &validada) != 0)
        return -1;

    /* 2) resultado com os metadados da projecao */
    if (qr_iniciar(resultado, validada.tabela, projecao, n_projecao) != 0)
        return ex_erro(ex, "nao foi possivel iniciar o resultado da tabela '%s'",
                       validada.tabela->nome);

    /* 3..8) varredura, WHERE, projecao e montagem do resultado */
    if (executor_varrer(ex, cat, comando, validada.tabela, resultado) != 0) {
        qr_liberar(resultado);
        return -1;
    }
    return resultado->numero_linhas;
}

/* ------------------------------------------------------------------ */
/* Demonstracao                                                        */
/* ------------------------------------------------------------------ */

/* Parseia 'sql', executa e imprime o resultado ou o erro. */
static void demo_consulta(Executor *ex, Catalogo *cat, const char *sql,
                          const char *const *projecao, int n_projecao)
{
    Parser      parser;
    Comando     comando;
    QueryResult resultado;

    printf("\n> %s", sql);
    if (projecao != NULL && n_projecao > 0) {
        printf("   [projecao:");
        for (int i = 0; i < n_projecao; i++)
            printf("%s %s", i ? "," : "", projecao[i] != NULL ? projecao[i] : "(null)");
        printf("]");
    }
    printf("\n");

    iniciar_parser(&parser, sql);
    if (!parse_comando(&parser, &comando)) {
        printf("  ERRO de sintaxe: %s\n", parser.mensagem);
        return;
    }

    if (executar_consulta(ex, cat, &comando, projecao, n_projecao, &resultado) < 0) {
        printf("  ERRO: %s\n", ex->mensagem);
        return;
    }

    qr_imprimir(&resultado);
    printf("  (varridas: %d | descartadas pelo WHERE: %d | retornadas: %d)\n",
           ex->linhas_varridas, ex->linhas_descartadas, qr_numero_linhas(&resultado));
    qr_liberar(&resultado);
}
#ifdef QUERY_EXECUTOR_SEM_MAIN
int main(void)
{
    static Catalogo cat;
    Executor ex;
    Coluna   colunas[2];
    Row      linha;

    remove("alunos.dat");               /* parte de um arquivo de dados limpo */
    catalogo_iniciar(&cat);
    iniciar_executor(&ex);

    /* 1) Catalogo: alunos (id INT, nome CHAR) */
    strcpy(colunas[0].nome, "id");   colunas[0].tipo = TOKEN_INT;
    strcpy(colunas[1].nome, "nome"); colunas[1].tipo = TOKEN_CHAR;
    if (!catalogo_adicionar_tabela(&cat, "alunos", colunas, 2)) {
        fprintf(stderr, "Erro no catalogo: %s\n", cat.mensagem);
        return 1;
    }
    const TabelaCat *alunos = catalogo_buscar_tabela(&cat, "alunos");

    /* 2) Carga fora de ordem (forca divisoes de folha na arvore B+) */
    const int32_t ids[]   = { 50, 10, 80, 30, 70, 20, 60, 40, 90, 100 };
    const char   *nomes[] = { "Eva", "Ana", "Iris", "Caio", "Hugo",
                              "Bia", "Gil", "Davi", "Joao", "Kai" };
    const int quantidade = (int)(sizeof ids / sizeof ids[0]);

    for (int i = 0; i < quantidade; i++) {
        row_iniciar(&linha, alunos);
        row_definir_int(&linha, 0, ids[i]);
        row_definir_char(&linha, 1, nomes[i]);
        if (row_inserir(&cat, &linha) != INSERE_OK) {
            fprintf(stderr, "Falha ao inserir a linha %d\n", i);
            return 1;
        }
    }
    printf("%d linhas inseridas em 'alunos' (fora de ordem).\n", quantidade);

    /* 3) SELECT * sem WHERE: todas as linhas, em ordem crescente da chave */
    printf("\n=== Sem WHERE / com WHERE ===");
    demo_consulta(&ex, &cat, "SELECT * FROM alunos;", NULL, 0);
    demo_consulta(&ex, &cat, "SELECT * FROM alunos WHERE id >= 30 AND id < 80;", NULL, 0);
    demo_consulta(&ex, &cat, "SELECT * FROM alunos WHERE nome <> 'Ana' AND id > 60;", NULL, 0);
    demo_consulta(&ex, &cat, "SELECT * FROM alunos WHERE id > 1000;", NULL, 0);

    /* 4) Projecao de colunas: ordem pedida preservada, nomes sem case */
    printf("\n\n=== Projecao de colunas ===");
    const char *proj_nome_id[] = { "nome", "ID" };
    const char *proj_nome[]    = { "nome" };
    demo_consulta(&ex, &cat, "SELECT * FROM alunos WHERE id <= 30;", proj_nome_id, 2);
    demo_consulta(&ex, &cat, "SELECT * FROM alunos WHERE id = 70;",  proj_nome, 1);

    /* 5) Consultas rejeitadas: nenhuma varredura chega a ser aberta */
    printf("\n\n=== Consultas rejeitadas ===");
    const char *proj_ruim[] = { "id", "idade" };
    demo_consulta(&ex, &cat, "SELECT * FROM fantasma;", NULL, 0);
    demo_consulta(&ex, &cat, "SELECT * FROM alunos WHERE idade = 3;", NULL, 0);
    demo_consulta(&ex, &cat, "SELECT * FROM alunos WHERE id = 'abc';", NULL, 0);
    demo_consulta(&ex, &cat, "SELECT * FROM alunos;", proj_ruim, 2);
    demo_consulta(&ex, &cat, "INSERT INTO alunos VALUES (1, 'Zed');", NULL, 0);

    /* 6) Nenhum pin vazado: o cache so pode ser reiniciado sem paginas fixadas */
    printf("\n\nreiniciar_cache() apos as consultas -> %s\n",
           reiniciar_cache() == 0 ? "aceito (nenhum pin vazado)" : "recusado (pin vazado!)");

    remove("alunos.dat");
    return 0;
}

#endif /* QUERY_EXECUTOR_SEM_MAIN */

#endif /* QUERY_EXECUTOR */
