/* ============================================================
 *                 FILTER (OPERADOR DE FILTRAGEM)
 * ============================================================
 *
 * Operador do modelo iterador que consome as tuplas de um operador FILHO e
 * entrega apenas as que satisfazem um PREDICADO:
 *
 *      filter_abrir()  ->  filter_next() ... filter_next()  ->  filter_fechar()
 *
 * O filter nao sabe de onde as tuplas vem (table_scan, outro filter, ...): ele
 * so conhece a interface Operador (abrir / next / fechar). Tambem nao sabe como
 * o predicado e' avaliado: delega ao predicate_evaluator (linha_permanece).
 *
 * O que o filter guarda (filter_iniciar):
 *   - filho      : o operador de onde as tuplas sao puxadas;
 *   - predicado  : arvore de predicados (NULL = sem filtro, tudo passa);
 *   - avaliador  : onde ficam a mensagem de erro de avaliacao.
 *
 * Ciclo de vida:
 *   filter_iniciar : so guarda as referencias; NAO abre o filho.
 *   filter_abrir   : abre o filho e prepara o estado de execucao (contadores,
 *                    flags). Se o filho falhar ao abrir, o filter continua fechado.
 *   filter_next    : puxa tuplas do filho ate achar a PRIMEIRA que satisfaz o
 *                    predicado e a devolve. A proxima chamada continua de onde
 *                    parou (uma tupla por chamada).
 *   filter_fechar  : fecha o filho (liberando pins/recursos dele) e zera o
 *                    estado. Pode ser chamada varias vezes com seguranca.
 *
 * Propriedade da memoria: nao ha malloc. O filter NAO e' dono do filho, do
 * predicado nem do avaliador; o chamador os mantem vivos (e sem mover) entre
 * filter_iniciar e filter_fechar.
 *
 * Erros: erro de avaliacao do predicado NUNCA vira "falso" em silencio: a
 * varredura termina com -1 e a mensagem fica em avaliador->mensagem.
 * Depois de um erro o chamador deve chamar filter_fechar() para soltar o filho.
 *
 * Convencao de retorno (igual a do table_scan):
 *   filter_iniciar / filter_abrir / filter_fechar : 0 = sucesso, -1 = erro
 *   filter_next : 1 = tupla entregue, 0 = fim, -1 = erro
 *
 * Compilacao:   gcc -Wall -Wextra -o filter filter.c
 * Como biblioteca (sem o main do final):  -DFILTER_SEM_MAIN
 */

#ifndef FILTER
#define FILTER

/* table_scan.c traz row.c (com o main renomeado) e deve vir primeiro. */
#include "../scan/table_scan.c"
#include "../evaluator/predicate_evaluator.c"

/* ------------------------------------------------------------------ */
/* Interface generica de operador                                      */
/* ------------------------------------------------------------------ */

/*
 Um operador e' um contexto opaco + tres funcoes. Qualquer operador que siga
 esta interface pode ser filho do filter (e o proprio filter pode ser filho de
 outro, ver operador_de_filter).
*/
typedef struct Operador Operador;
struct Operador {
    void *contexto;
    int (*abrir)(void *contexto);                   /* 0 ou -1             */
    int (*next)(void *contexto, Row *saida);        /* 1, 0 (fim) ou -1    */
    int (*fechar)(void *contexto);                  /* 0 ou -1             */
};

static int operador_completo(const Operador *op)
{
    return op != NULL && op->abrir != NULL && op->next != NULL && op->fechar != NULL;
}

/* ------------------------------------------------------------------ */
/* Estado do filter                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    /* Entradas guardadas em filter_iniciar (o filter nao e' dono delas). */
    Operador        *filho;
    const Predicado *predicado;
    Avaliador       *avaliador;

    /* Estado de execucao, preparado em filter_abrir. */
    int              aberto;            /* 1 entre filter_abrir e filter_fechar     */
    int              terminado;         /* 1 apos o fim do filho ou apos um erro    */
    int              linhas_lidas;      /* tuplas puxadas do filho                  */
    int              linhas_descartadas;/* tuplas que nao satisfizeram o predicado  */
} Filter;

/* ------------------------------------------------------------------ */
/* Interface publica                                                   */
/* ------------------------------------------------------------------ */

/*
 filter_iniciar(filtro, filho, predicado, avaliador)
 Guarda o operador filho, o predicado e o avaliador. Nao abre nada.
 'predicado' pode ser NULL (sem filtro). 'filho' deve ter as tres funcoes
 preenchidas e 'avaliador' nao pode ser NULL.
 Retorna 0 em caso de sucesso, ou -1 em caso de erro.
*/
int filter_iniciar(Filter *filtro, Operador *filho, const Predicado *predicado,
                   Avaliador *avaliador)
{
    if (filtro == NULL) {
        fprintf(stderr, "Erro: filter_iniciar recebeu filtro NULL\n");
        return -1;
    }
    memset(filtro, 0, sizeof *filtro);

    if (!operador_completo(filho)) {
        fprintf(stderr, "Erro: filter_iniciar recebeu operador filho NULL ou incompleto\n");
        return -1;
    }
    if (avaliador == NULL) {
        fprintf(stderr, "Erro: filter_iniciar recebeu avaliador NULL\n");
        return -1;
    }

    filtro->filho     = filho;
    filtro->predicado = predicado;
    filtro->avaliador = avaliador;
    return 0;
}

/*
 filter_abrir(filtro)
 Abre o operador filho e zera o estado de execucao. Reabrir um filter ja
 aberto e' erro (feche antes).
 Retorna 0 em caso de sucesso, ou -1 em caso de erro.
*/
int filter_abrir(Filter *filtro)
{
    if (filtro == NULL || !operador_completo(filtro->filho) || filtro->avaliador == NULL) {
        fprintf(stderr, "Erro: filter_abrir chamado em filtro nao iniciado\n");
        return -1;
    }
    if (filtro->aberto) {
        fprintf(stderr, "Erro: filter_abrir chamado em filtro ja aberto\n");
        return -1;
    }

    iniciar_avaliador(filtro->avaliador);
    filtro->terminado          = 0;
    filtro->linhas_lidas       = 0;
    filtro->linhas_descartadas = 0;

    if (filtro->filho->abrir(filtro->filho->contexto) != 0) {
        fprintf(stderr, "Erro: filter nao conseguiu abrir o operador filho\n");
        return -1;
    }
    filtro->aberto = 1;
    return 0;
}

/*
 filter_next(filtro, saida)
 Puxa tuplas do filho, descartando as que nao satisfazem o predicado, e entrega
 em *saida a primeira que satisfaz. Chamadas seguintes continuam dali.
 Depois do fim (ou de um erro), novas chamadas continuam devolvendo 0.
 Retorna 1 se *saida recebeu uma tupla, 0 no fim, ou -1 em erro (erro de
 avaliacao: mensagem em avaliador->mensagem).
*/
int filter_next(Filter *filtro, Row *saida)
{
    if (filtro == NULL || saida == NULL) {
        fprintf(stderr, "Erro: filter_next recebeu argumento NULL\n");
        return -1;
    }
    if (!filtro->aberto) {
        fprintf(stderr, "Erro: filter_next chamado em filtro nao aberto\n");
        return -1;
    }

    while (!filtro->terminado) {
        int r = filtro->filho->next(filtro->filho->contexto, saida);

        if (r < 0) {
            filtro->terminado = 1;
            return -1;
        }
        if (r == 0) {                       /* filho esgotado: fim real */
            filtro->terminado = 1;
            break;
        }
        filtro->linhas_lidas++;

        int permanece = 0;
        if (linha_permanece(filtro->avaliador, filtro->predicado, saida, &permanece) != 0) {
            fprintf(stderr, "Erro: filter: falha ao avaliar o predicado: %s\n",
                    filtro->avaliador->mensagem);
            filtro->terminado = 1;
            return -1;
        }
        if (permanece)
            return 1;

        filtro->linhas_descartadas++;
    }
    return 0;
}

/*
 filter_fechar(filtro)
 Fecha o operador filho (que libera os recursos dele, como pins de cache) e
 encerra o filter. Segura para chamar mais de uma vez e apos erros; as
 referencias guardadas continuam validas para um novo filter_abrir.
 Retorna 0 em caso de sucesso, ou -1 em caso de erro.
*/
int filter_fechar(Filter *filtro)
{
    int status = 0;

    if (filtro == NULL)
        return -1;

    if (filtro->aberto) {
        if (filtro->filho->fechar(filtro->filho->contexto) != 0) {
            fprintf(stderr, "Erro: filter nao conseguiu fechar o operador filho\n");
            status = -1;
        }
        filtro->aberto = 0;
    }
    filtro->terminado = 1;
    return status;
}

/* ------------------------------------------------------------------ */
/* Adaptadores para a interface Operador                               */
/* ------------------------------------------------------------------ */

/* O filter como operador: permite empilhar filtros (filter sobre filter). */
static int filter_op_abrir(void *ctx)             { return filter_abrir((Filter *)ctx); }
static int filter_op_next(void *ctx, Row *saida)  { return filter_next((Filter *)ctx, saida); }
static int filter_op_fechar(void *ctx)            { return filter_fechar((Filter *)ctx); }

static void operador_de_filter(Operador *op, Filter *filtro)
{
    op->contexto = filtro;
    op->abrir    = filter_op_abrir;
    op->next     = filter_op_next;
    op->fechar   = filter_op_fechar;
}

/* O table_scan como operador. O contexto guarda o que table_scan_abrir exige. */
typedef struct {
    TableScan  scan;
    Catalogo  *cat;
    char       tabela[MAX_LEXEMA];
} ContextoTableScan;

static int scan_op_abrir(void *ctx)
{
    ContextoTableScan *c = (ContextoTableScan *)ctx;
    return table_scan_abrir(&c->scan, c->cat, c->tabela);
}

static int scan_op_next(void *ctx, Row *saida)
{
    return table_scan_next(&((ContextoTableScan *)ctx)->scan, saida);
}

static int scan_op_fechar(void *ctx)
{
    return table_scan_fechar(&((ContextoTableScan *)ctx)->scan);
}

static void operador_de_table_scan(Operador *op, ContextoTableScan *ctx,
                                   Catalogo *cat, const char *tabela)
{
    memset(ctx, 0, sizeof *ctx);
    ctx->cat = cat;
    snprintf(ctx->tabela, sizeof ctx->tabela, "%s", tabela != NULL ? tabela : "");

    op->contexto = ctx;
    op->abrir    = scan_op_abrir;
    op->next     = scan_op_next;
    op->fechar   = scan_op_fechar;
}

/* ------------------------------------------------------------------ */
/* Demonstracao                                                        */
/* ------------------------------------------------------------------ */

/* Executa filter(table_scan(tabela), predicado) ate o fim e imprime as tuplas. */
static void demo_filtro(const char *rotulo, Catalogo *cat, const Predicado *predicado)
{
    ContextoTableScan ctx_scan;
    Operador          scan;
    Avaliador         av;
    Filter            filtro;
    Row               linha;
    int               r, entregues = 0;

    printf("\n> %s\n", rotulo);

    operador_de_table_scan(&scan, &ctx_scan, cat, "alunos");
    iniciar_avaliador(&av);
    if (filter_iniciar(&filtro, &scan, predicado, &av) != 0 || filter_abrir(&filtro) != 0) {
        printf("  ERRO ao preparar o filtro\n");
        return;
    }

    while ((r = filter_next(&filtro, &linha)) == 1) {
        printf("  ");
        row_imprimir(&linha);
        entregues++;
    }
    if (r < 0)
        printf("  ERRO durante a filtragem: %s\n", av.mensagem);

    printf("  (lidas: %d | descartadas: %d | entregues: %d)\n",
           filtro.linhas_lidas, filtro.linhas_descartadas, entregues);
    filter_fechar(&filtro);
}
#ifdef FILTER_SEM_MAIN
int main(void)
{
    static Catalogo cat;
    Coluna colunas[2];
    Row    linha;

    remove("alunos.dat");               /* parte de um arquivo de dados limpo */
    catalogo_iniciar(&cat);

    /* 1) Catalogo e carga fora de ordem (forca divisoes de folha) */
    strcpy(colunas[0].nome, "id");   colunas[0].tipo = TOKEN_INT;
    strcpy(colunas[1].nome, "nome"); colunas[1].tipo = TOKEN_CHAR;
    if (!catalogo_adicionar_tabela(&cat, "alunos", colunas, 2)) {
        fprintf(stderr, "Erro no catalogo: %s\n", cat.mensagem);
        return 1;
    }
    const TabelaCat *alunos = catalogo_buscar_tabela(&cat, "alunos");

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

    /* 2) Predicados (os operandos ficam vivos ate o fim do main) */
    Expressao id     = expr_coluna("id");
    Expressao nome   = expr_coluna("nome");
    Expressao idade  = expr_coluna("idade");               /* coluna inexistente */
    Expressao n30    = expr_literal(valor_int(30));
    Expressao n80    = expr_literal(valor_int(80));
    Expressao n1000  = expr_literal(valor_int(1000));
    Expressao ana    = expr_literal(valor_char("Ana"));

    Predicado id_ge_30   = pred_comparacao(TOKEN_MAIOR_IGUAL, &id,   &n30);
    Predicado id_lt_80   = pred_comparacao(TOKEN_MENOR,       &id,   &n80);
    Predicado id_gt_1000 = pred_comparacao(TOKEN_MAIOR,       &id,   &n1000);
    Predicado nome_ne    = pred_comparacao(TOKEN_DIFERENTE,   &nome, &ana);
    Predicado idade_ok   = pred_comparacao(TOKEN_IGUAL,       &idade, &n30);
    Predicado faixa      = pred_and(&id_ge_30, &id_lt_80);
    Predicado faixa_nome = pred_and(&faixa, &nome_ne);

    printf("\n=== Filter sobre TableScan ===");
    demo_filtro("sem predicado (tudo passa)",           &cat, NULL);
    demo_filtro("id >= 30 AND id < 80",                 &cat, &faixa);
    demo_filtro("id >= 30 AND id < 80 AND nome <> 'Ana'", &cat, &faixa_nome);
    demo_filtro("id > 1000 (nenhuma tupla satisfaz)",   &cat, &id_gt_1000);

    /* 3) Erro de avaliacao: coluna inexistente aborta, nao vira "falso" */
    printf("\n=== Erro de avaliacao ===");
    demo_filtro("idade = 30 (coluna inexistente)",      &cat, &idade_ok);

    /* 4) filter sobre filter: id >= 30 por baixo, id < 80 por cima */
    printf("\n\n=== filter sobre filter ===\n");
    {
        ContextoTableScan ctx_scan;
        Operador          scan, interno_op;
        Avaliador         av_interno, av_externo;
        Filter            interno, externo;
        Row               tupla;
        int               r, n = 0;

        operador_de_table_scan(&scan, &ctx_scan, &cat, "alunos");
        iniciar_avaliador(&av_interno);
        iniciar_avaliador(&av_externo);

        if (filter_iniciar(&interno, &scan, &id_ge_30, &av_interno) != 0) return 1;
        operador_de_filter(&interno_op, &interno);
        if (filter_iniciar(&externo, &interno_op, &id_lt_80, &av_externo) != 0) return 1;

        if (filter_abrir(&externo) != 0) return 1;
        while ((r = filter_next(&externo, &tupla)) == 1) {
            printf("  ");
            row_imprimir(&tupla);
            n++;
        }
        printf("  %d tupla(s); next() final -> %d (esperado 0)\n", n, r);
        filter_fechar(&externo);
    }

    /* 5) Desistencia no meio: fechar o filter libera o pin do table_scan filho */
    printf("\n=== Fechamento antecipado ===\n");
    {
        ContextoTableScan ctx_scan;
        Operador          scan;
        Avaliador         av;
        Filter            filtro;
        Row               tupla;

        operador_de_table_scan(&scan, &ctx_scan, &cat, "alunos");
        iniciar_avaliador(&av);
        if (filter_iniciar(&filtro, &scan, &id_ge_30, &av) != 0 || filter_abrir(&filtro) != 0)
            return 1;

        if (filter_next(&filtro, &tupla) == 1) {
            printf("  primeira tupla: ");
            row_imprimir(&tupla);
        }
        printf("  reiniciar_cache() com filtro aberto -> %s\n",
               reiniciar_cache() != 0 ? "recusado (pagina fixada)" : "aceito");
        filter_fechar(&filtro);
        printf("  reiniciar_cache() apos filter_fechar() -> %s\n",
               reiniciar_cache() == 0 ? "aceito (pin liberado)" : "recusado (pin vazado!)");
        printf("  segundo filter_fechar() -> %d (esperado 0)\n", filter_fechar(&filtro));
    }

    /* 6) Uso incorreto */
    printf("\n=== Uso incorreto ===\n");
    {
        Filter filtro;
        Avaliador av;
        Row tupla;

        iniciar_avaliador(&av);
        printf("  filter_iniciar com filho NULL -> %d (esperado -1)\n",
               filter_iniciar(&filtro, NULL, NULL, &av));
        printf("  filter_next sem abrir         -> %d (esperado -1)\n",
               filter_next(&filtro, &tupla));
    }

    remove("alunos.dat");
    return 0;
}

#endif /* FILTER_SEM_MAIN */

#endif /* FILTER */
