/* ============================================================
 *                 EXECUTION PLAN (PLANO DE EXECUCAO)
 * ============================================================
 *
 * Decide COMO um SELECT sera executado: por Index Scan (arvore B+) ou por Table
 * Scan (varredura completa). O modulo so decide e executa o que foi decidido;
 * quem sabe percorrer o indice ou a tabela continuam sendo index_scan.c e
 * table_scan.c (cada modulo com UMA responsabilidade).
 *
 * Pipeline do planejador (plano_criar):
 *
 *   1) VALIDAR      SELECT, tabela e colunas/tipos do WHERE conferidos no catalogo
 *   2) SEM WHERE    nada a filtrar pelo indice -> Table Scan (sem tocar no disco)
 *   3) INDICES      catalogo_listar_indices() + planejar_index_scan() (index_scan.c)
 *                   dizem se ha indice EXECUTAVEL e qual faixa [inferior, superior]
 *                   da chave o WHERE permite. A analise de predicados indexaveis
 *                   NAO e' repetida aqui: e' reaproveitada.
 *   4) NAO INDEXAVEL  nenhum predicado serve ao indice -> Table Scan
 *   5) FAIXA VAZIA    contradicao (id > 90 AND id < 20) -> Index Scan, custo zero
 *   6) ESTATISTICAS   so metadados e 2 descidas na arvore (ver abaixo)
 *   7) CUSTO          estima o custo dos dois metodos e escolhe o MENOR
 *
 * Modelo de custo (unidades de "leitura de pagina"):
 *
 *   Table Scan : (altura + folhas) * CUSTO_PAGINA + linhas * CUSTO_TUPLA
 *                (pagina de metadados + descida pela esquerda + cadeia de folhas)
 *   Index Scan : (1 + altura + folhas_extras_da_faixa) * CUSTO_PAGINA
 *                + linhas_da_faixa * (CUSTO_TUPLA + CUSTO_REFERENCIA)
 *                (pagina de metadados + descida ate o limite inferior + folhas
 *                 que ainda cobrem a faixa)
 *
 *   - O Index Scan so visita as folhas que cobrem a faixa; em troca, cada linha
 *     passa por referencia (fixar/soltar a pagina e desserializar), mais cara
 *     que a leitura sequencial do Table Scan.
 *   - Resultado pratico: igualdade e faixas estreitas -> Index Scan; faixa que
 *     cobre quase toda a tabela, ou tabela minuscula (a descida nao compensa)
 *     -> Table Scan. Empate -> Table Scan (mais simples).
 *
 * Estatisticas (plano_coletar_estatisticas), sem varrer a tabela:
 *   - altura       : niveis da raiz ate a folha (contados na descida);
 *   - chave min/max: 1o slot da folha mais a esquerda e ultimo da mais a direita;
 *   - folhas       : estimadas a partir de total_paginas_alocadas() descontando os
 *                    nos internos esperados. Medido nesta B+ (MAX_CHAVES = 3): os
 *                    internos ficam com ~3 dos 4 filhos possiveis (FATOR_FANOUT) e as
 *                    folhas com ~2 a 2,8 registros (FATOR_PREENCHIMENTO);
 *   - linhas       : folhas * MAX_CHAVES * FATOR_PREENCHIMENTO (exato se a raiz
 *                    for a unica folha);
 *   - selectividade: distribuicao uniforme das chaves entre min e max.
 *   Sao ESTIMATIVAS. Um erro de estimativa so afeta o desempenho, nunca o
 *   resultado: o Index Scan reavalia o WHERE inteiro e os dois metodos entregam
 *   as mesmas linhas na mesma ordem (crescente da chave).
 *
 * Execucao (CursorPlano), no modelo iterador dos demais operadores:
 *
 *      plano_abrir()  ->  plano_next() ... plano_next()  ->  plano_fechar()
 *
 *   - INDEX_SCAN : delega a index_scan_*, que ja reavalia o WHERE.
 *   - TABLE_SCAN : delega a table_scan_* e aplica linha_permanece_where().
 *   Se o Index Scan responder "nao aplicavel" ao abrir, o cursor degrada para
 *   Table Scan (cursor->degradado = 1) em vez de falhar.
 *
 * Restricoes herdadas do Pager (ha um unico arquivo corrente):
 *   - plano_criar() e plano_abrir() chamam selecionar_arquivo_tabela(), que
 *     reinicia o cache: nenhuma varredura pode estar aberta nesse momento;
 *   - use UMA varredura ativa por vez e nao acesse outra tabela entre
 *     plano_abrir() e plano_fechar();
 *   - o Catalogo e o Comando devem continuar vivos (e sem mover) ate
 *     plano_fechar().
 *
 * Convencao de retorno:
 *   plano_criar / plano_abrir / plano_fechar : 0 = sucesso, -1 = erro
 *   plano_next                               : 1 = tupla, 0 = fim, -1 = erro
 *   A mensagem de erro fica em PlanoExecucao.mensagem / CursorPlano.mensagem.
 *
 * Compilacao (a partir da raiz do projeto; includes relativos a src/):
 *   gcc -Wall -Wextra -Isrc -DEXECUTION_PLAN_MAIN -o plano <pasta>/query/execution_plan.c
 * (o arquivo fica numa pasta irma de "scan", como query_executor.c).
 * Como biblioteca, compile sem -DEXECUTION_PLAN_MAIN.
 */

#ifndef EXECUTION_PLAN
#define EXECUTION_PLAN

#include "../scan/index_scan.c"     /* table_scan.c + predicate_evaluator.c + catalogo + B+ */

/* ------------------------------------------------------------------ */
/* Parametros do modelo de custo (ajuste conforme medicoes reais)      */
/* ------------------------------------------------------------------ */

#define PLANO_CUSTO_PAGINA          1.00   /* ler uma pagina                          */
#define PLANO_CUSTO_TUPLA           0.01   /* desserializar + avaliar o WHERE         */
#define PLANO_CUSTO_REFERENCIA      0.05   /* extra do Index Scan: fixar/soltar pagina */
#define PLANO_FATOR_PREENCHIMENTO   0.70   /* ocupacao media de uma folha B+          */
#define PLANO_FATOR_FANOUT          0.75   /* filhos medios de um no interno / maximo */

/* ------------------------------------------------------------------ */
/* Tipos                                                               */
/* ------------------------------------------------------------------ */

typedef enum {
    ACESSO_TABLE_SCAN,
    ACESSO_INDEX_SCAN
} MetodoAcesso;

/* Por que o metodo foi escolhido (texto em plano_texto_motivo). */
typedef enum {
    MOTIVO_SEM_WHERE,                   /* nada a filtrar                          */
    MOTIVO_SEM_PREDICADO_INDEXAVEL,     /* nenhum indice executavel serve ao WHERE */
    MOTIVO_TABELA_VAZIA,
    MOTIVO_FAIXA_VAZIA,                 /* WHERE contraditorio: zero linhas        */
    MOTIVO_BUSCA_PONTUAL,               /* igualdade na chave                      */
    MOTIVO_FORA_DO_DOMINIO,             /* faixa fora das chaves existentes        */
    MOTIVO_INDEX_MAIS_BARATO,
    MOTIVO_TABLE_MAIS_BARATO
} MotivoEscolha;

typedef struct {
    int     vazia;              /* 1 = a tabela nao tem nenhuma linha     */
    int     paginas_dados;      /* paginas alocadas, sem a de metadados   */
    int     altura;             /* niveis da raiz ate a folha (>= 1)      */
    int     folhas;             /* estimado (exato se altura == 1)        */
    double  linhas;             /* estimado (exato se altura == 1)        */
    int32_t chave_min;
    int32_t chave_max;
} EstatisticasTabela;

typedef struct {
    MetodoAcesso       metodo;           /* decisao                              */
    MotivoEscolha      motivo;
    const TabelaCat   *tabela;
    const Comando     *comando;          /* WHERE original                       */

    PlanoIndexScan     indice;           /* melhor faixa de indice (pode nao ser a escolhida) */
    EstatisticasTabela stats;

    int                custos_calculados;/* 0 = decidido por regra, sem estimar  */
    double             selectividade;    /* fracao de linhas na faixa do indice  */
    double             linhas_faixa;     /* linhas estimadas na faixa            */
    double             custo_table_scan;
    double             custo_index_scan;

    char               mensagem[MAX_MESSAGEM];
} PlanoExecucao;

/* ------------------------------------------------------------------ */
/* Utilitarios                                                         */
/* ------------------------------------------------------------------ */

/* Grava a mensagem e devolve sempre -1, permitindo "return plano_erro(...)". */
static int plano_erro(PlanoExecucao *plano, const char *formato, ...)
{
    va_list ap;
    va_start(ap, formato);
    vsnprintf(plano->mensagem, sizeof plano->mensagem, formato, ap);
    va_end(ap);
    return -1;
}

static const char *plano_texto_motivo(MotivoEscolha motivo)
{
    switch (motivo) {
        case MOTIVO_SEM_WHERE:
            return "consulta sem WHERE: toda a tabela sera lida";
        case MOTIVO_SEM_PREDICADO_INDEXAVEL:
            return "nenhum predicado do WHERE pode usar um indice executavel";
        case MOTIVO_TABELA_VAZIA:
            return "tabela vazia: nada a procurar";
        case MOTIVO_FAIXA_VAZIA:
            return "faixa da chave contraditoria: resultado vazio, sem percorrer o indice";
        case MOTIVO_BUSCA_PONTUAL:
            return "igualdade na chave: descida direta na arvore e' mais barata";
        case MOTIVO_FORA_DO_DOMINIO:
            return "faixa fora das chaves existentes: so a descida na arvore e' necessaria";
        case MOTIVO_INDEX_MAIS_BARATO:
            return "o indice le menos paginas que a varredura completa";
        case MOTIVO_TABLE_MAIS_BARATO:
            return "indice aplicavel, mas a faixa e' larga ou a tabela e' pequena: "
                   "varrer a tabela custa menos";
    }
    return "?";
}

static double plano_teto(double x)
{
    int inteiro = (int)x;
    return (inteiro < x) ? inteiro + 1 : inteiro;
}

/* ------------------------------------------------------------------ */
/* Etapa 6: estatisticas                                               */
/* ------------------------------------------------------------------ */

/* Desce da raiz ate a folha de 'chave' (INT32_MIN = mais a esquerda, INT32_MAX =
   mais a direita). Devolve a COPIA da folha e a altura (niveis percorridos). */
static int plano_descer_ate_folha(int32_t chave, No *folha, int *altura)
{
    int pagina = ler_raiz();
    int limite = total_paginas_alocadas();
    int passos;
    No  no;

    for (passos = 0; ; passos++) {
        if (pagina < 0 || passos > limite || pagina <= PAGINA_METADADOS || pagina >= limite) {
            fprintf(stderr, "Erro: arvore corrompida na coleta de estatisticas (pagina %d)\n", pagina);
            return -1;
        }
        if (ler_no(pagina, &no) != 0)
            return -1;
        if (no.tipo == NO_FOLHA)
            break;
        pagina = no.filhos[indice_filho(&no, chave)];
    }
    *folha = no;
    if (altura != NULL)
        *altura = passos + 1;
    return 0;
}

/* Filhos medios de um no interno (nunca abaixo de 2, para a divisao terminar). */
static double plano_fanout_medio(void)
{
    double f = (MAX_CHAVES + 1) * PLANO_FATOR_FANOUT;
    return f < 2.0 ? 2.0 : f;
}

/* Nos internos esperados para uma arvore com 'folhas' folhas. */
static int plano_internos_para(int folhas)
{
    double fanout   = plano_fanout_medio();
    int    internos = 0;
    int    nivel    = folhas;

    while (nivel > 1) {
        nivel = (int)plano_teto(nivel / fanout);
        internos += nivel;
    }
    return internos;
}

/* Maior numero de folhas L tal que L + internos(L) cabe em 'paginas_dados'
   (a soma cresce com L, entao basta uma busca binaria). */
static int plano_estimar_folhas(int paginas_dados, int altura)
{
    int lo = 1, hi = paginas_dados;

    if (altura <= 1 || paginas_dados <= 1)
        return 1;

    while (lo < hi) {
        int meio = lo + (hi - lo + 1) / 2;
        if (meio + plano_internos_para(meio) <= paginas_dados)
            lo = meio;
        else
            hi = meio - 1;
    }
    return lo;
}

/* Exige o arquivo da tabela ja selecionado (selecionar_arquivo_tabela). */
static int plano_coletar_estatisticas(EstatisticasTabela *st)
{
    No esquerda, direita;

    memset(st, 0, sizeof *st);

    if (plano_descer_ate_folha(INT32_MIN, &esquerda, &st->altura) != 0)
        return -1;

    st->paginas_dados = total_paginas_alocadas() - (PAGINA_METADADOS + 1);
    if (st->paginas_dados < 1)
        st->paginas_dados = 1;

    if (esquerda.numero_chaves <= 0) {
        st->vazia = 1;
        return 0;
    }

    if (st->altura <= 1) {
        direita = esquerda;                         /* a raiz e' a unica folha */
    } else if (plano_descer_ate_folha(INT32_MAX, &direita, NULL) != 0) {
        return -1;
    }
    if (direita.numero_chaves <= 0) {
        fprintf(stderr, "Erro: folha mais a direita vazia numa arvore nao vazia\n");
        return -1;
    }

    st->chave_min = esquerda.registros[0].id;
    st->chave_max = direita.registros[direita.numero_chaves - 1].id;

    if (st->altura <= 1) {
        st->folhas = 1;
        st->linhas = esquerda.numero_chaves;
    } else {
        st->folhas = plano_estimar_folhas(st->paginas_dados, st->altura);
        st->linhas = st->folhas * (double)MAX_CHAVES * PLANO_FATOR_PREENCHIMENTO;
        if (st->linhas < 1.0)
            st->linhas = 1.0;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Etapa 7: custos e decisao                                           */
/* ------------------------------------------------------------------ */

/* Preenche selectividade, linhas_faixa e os dois custos de 'p'. Exige stats nao vazia. */
static void plano_estimar_custos(PlanoExecucao *p)
{
    const EstatisticasTabela *st = &p->stats;
    const PlanoIndexScan     *ix = &p->indice;

    int64_t lo = ix->inferior > st->chave_min ? ix->inferior : (int64_t)st->chave_min;
    int64_t hi = ix->superior < st->chave_max ? ix->superior : (int64_t)st->chave_max;
    double  linhas = 0.0;
    double  extras = 0.0;           /* folhas alem da primeira (ja lida na descida) */

    if (lo <= hi) {                 /* a faixa cruza o dominio das chaves */
        double largura   = (double)((int64_t)st->chave_max - (int64_t)st->chave_min + 1);
        double cobertura = (double)(hi - lo + 1);

        linhas = (cobertura / largura) * st->linhas;
        if (ix->pontual && ix->indice.unico)
            linhas = 1.0;           /* chave unica: no maximo uma linha */
        if (linhas < 1.0)
            linhas = 1.0;
        if (linhas > st->linhas)
            linhas = st->linhas;

        extras = plano_teto((linhas / st->linhas) * st->folhas) - 1.0;
        if (extras < 0.0)
            extras = 0.0;
    }

    p->linhas_faixa  = linhas;
    p->selectividade = linhas / st->linhas;

    /* Ambos leem a pagina de metadados (1) e descem ate uma folha (altura). */
    p->custo_table_scan = (st->altura + st->folhas) * PLANO_CUSTO_PAGINA
                        + st->linhas * PLANO_CUSTO_TUPLA;
    p->custo_index_scan = (1 + st->altura + extras) * PLANO_CUSTO_PAGINA
                        + linhas * (PLANO_CUSTO_TUPLA + PLANO_CUSTO_REFERENCIA);
    p->custos_calculados = 1;
}

/* Ponto unico da decisao: o menor custo vence; empate fica com o Table Scan. */
static void plano_decidir_por_custo(PlanoExecucao *p)
{
    if (p->custo_index_scan < p->custo_table_scan) {
        p->metodo = ACESSO_INDEX_SCAN;
        if (p->indice.pontual)         p->motivo = MOTIVO_BUSCA_PONTUAL;
        else if (p->linhas_faixa <= 0) p->motivo = MOTIVO_FORA_DO_DOMINIO;
        else                           p->motivo = MOTIVO_INDEX_MAIS_BARATO;
    } else {
        p->metodo = ACESSO_TABLE_SCAN;
        p->motivo = MOTIVO_TABLE_MAIS_BARATO;
    }
}

/* ------------------------------------------------------------------ */
/* Interface publica: planejamento                                     */
/* ------------------------------------------------------------------ */

/*
 plano_criar(plano, cat, comando)
 Valida o SELECT, escolhe Index Scan ou Table Scan e preenche 'plano'.
 Nao executa nada e nao deixa pin pendente (so le metadados e 2 caminhos da
 arvore, via ler_no, que fixa/copia/solta). Retorna 0 ou -1 (mensagem em
 plano->mensagem).
*/
int plano_criar(PlanoExecucao *plano, Catalogo *cat, const Comando *comando)
{
    if (plano == NULL)
        return -1;
    memset(plano, 0, sizeof *plano);
    plano->metodo           = ACESSO_TABLE_SCAN;     /* opcao segura por padrao */
    plano->custo_index_scan = -1.0;

    /* 1) validar */
    if (cat == NULL || comando == NULL)
        return plano_erro(plano, "plano_criar recebeu argumento NULL");
    if (comando->tipo != CMD_SELECT)
        return plano_erro(plano, "o plano de execucao so se aplica a SELECT");

    const TabelaCat *tabela = catalogo_buscar_tabela(cat, comando->tabela);
    if (tabela == NULL)
        return plano_erro(plano, "tabela '%s' nao existe no catalogo", comando->tabela);
    if (!catalogo_validar_select(cat, comando))
        return plano_erro(plano, "consulta invalida: %s", cat->mensagem);

    plano->tabela  = tabela;
    plano->comando = comando;

    /* 2) sem WHERE: decidido sem tocar no disco */
    if (comando->numero_condicoes == 0) {
        plano->motivo = MOTIVO_SEM_WHERE;
        return 0;
    }

    /* 3) indices do catalogo, operacoes da B+ e melhor faixa (index_scan.c) */
    ListaIndices indices;
    catalogo_listar_indices(cat, tabela, &indices);

    if (!selecionar_arquivo_tabela(cat, tabela))
        return plano_erro(plano, "%s", cat->mensagem);

    BPlusCapacidades capacidades;
    if (bplus_verificar_operacoes(&capacidades) != 0)
        return plano_erro(plano, "arvore B+ de '%s' ilegivel", tabela->nome);

    planejar_index_scan(tabela, comando, &indices, &capacidades, &plano->indice);

    /* 4) nenhum predicado serve ao indice */
    if (!plano->indice.usa_indice) {
        plano->motivo = MOTIVO_SEM_PREDICADO_INDEXAVEL;
        return 0;
    }

    /* 5) contradicao no WHERE: o Index Scan encerra sem ler dados */
    if (plano->indice.vazio) {
        plano->metodo           = ACESSO_INDEX_SCAN;
        plano->motivo           = MOTIVO_FAIXA_VAZIA;
        plano->custo_index_scan = 0.0;      /* decidido por regra: custos nao estimados */
        return 0;
    }

    /* 6) estatisticas */
    if (plano_coletar_estatisticas(&plano->stats) != 0)
        return plano_erro(plano, "nao foi possivel coletar estatisticas de '%s'", tabela->nome);
    if (plano->stats.vazia) {
        plano->motivo = MOTIVO_TABELA_VAZIA;
        return 0;
    }

    /* 7) custo e decisao */
    plano_estimar_custos(plano);
    plano_decidir_por_custo(plano);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Interface publica: execucao                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    const PlanoExecucao *plano;
    MetodoAcesso         metodo;         /* metodo EFETIVO (pode degradar)        */
    int                  degradado;      /* 1 = Index Scan nao se aplicou         */
    int                  aberto;

    TableScan            table_scan;
    IndexScan            index_scan;
    Avaliador            avaliador;      /* WHERE do Table Scan                   */

    long                 linhas_lidas;       /* tuplas avaliadas contra o WHERE   */
    long                 linhas_descartadas; /* reprovadas pelo WHERE             */
    long                 linhas_emitidas;

    char                 mensagem[MAX_MESSAGEM];
} CursorPlano;

static int cursor_erro(CursorPlano *c, const char *formato, ...)
{
    va_list ap;
    va_start(ap, formato);
    vsnprintf(c->mensagem, sizeof c->mensagem, formato, ap);
    va_end(ap);
    return -1;
}

/*
 plano_abrir(cursor, cat, plano)
 Abre o operador escolhido pelo plano. O plano, o catalogo e o comando devem
 continuar vivos ate plano_fechar(). Retorna 0 ou -1 (mensagem em cursor->mensagem).
*/
int plano_abrir(CursorPlano *c, Catalogo *cat, const PlanoExecucao *plano)
{
    if (c == NULL)
        return -1;
    memset(c, 0, sizeof *c);

    if (cat == NULL || plano == NULL || plano->tabela == NULL || plano->comando == NULL)
        return cursor_erro(c, "plano_abrir recebeu plano ou argumento invalido");

    c->plano  = plano;
    c->metodo = plano->metodo;
    iniciar_avaliador(&c->avaliador);

    if (c->metodo == ACESSO_INDEX_SCAN) {
        int r = index_scan_abrir(&c->index_scan, cat, plano->comando);
        if (r < 0)
            return cursor_erro(c, "nao foi possivel abrir o Index Scan em '%s'", plano->tabela->nome);
        if (r == INDEX_SCAN_NAO_APLICAVEL) {        /* nada foi aberto: cai no Table Scan */
            c->metodo    = ACESSO_TABLE_SCAN;
            c->degradado = 1;
        }
    }

    if (c->metodo == ACESSO_TABLE_SCAN) {
        if (table_scan_abrir(&c->table_scan, cat, plano->tabela->nome) != 0)
            return cursor_erro(c, "nao foi possivel abrir o Table Scan de '%s'", plano->tabela->nome);
    }

    c->aberto = 1;
    return 0;
}

/*
 plano_next(cursor, saida)
 Entrega a proxima linha que satisfaz o WHERE completo, em ordem crescente da
 chave (igual nos dois metodos). Retorna 1, 0 (fim) ou -1 (erro).
*/
int plano_next(CursorPlano *c, Row *saida)
{
    if (c == NULL || saida == NULL) {
        fprintf(stderr, "Erro: plano_next recebeu argumento NULL\n");
        return -1;
    }
    if (!c->aberto)
        return cursor_erro(c, "plano_next chamado em cursor nao aberto");

    if (c->metodo == ACESSO_INDEX_SCAN) {
        int r = index_scan_next(&c->index_scan, saida);      /* ja reavalia o WHERE */

        c->linhas_lidas       = c->index_scan.registros_lidos;
        c->linhas_descartadas = c->index_scan.descartadas;
        if (r < 0)
            return cursor_erro(c, "falha durante o Index Scan");
        if (r == 1)
            c->linhas_emitidas++;
        return r;
    }

    for (;;) {
        int permanece = 0;
        int r = table_scan_next(&c->table_scan, saida);

        if (r < 0)
            return cursor_erro(c, "falha durante o Table Scan");
        if (r == 0)
            return 0;
        c->linhas_lidas++;

        /* erro de avaliacao nunca vira "falso" em silencio */
        if (linha_permanece_where(&c->avaliador, c->plano->comando, saida, &permanece) != 0)
            return cursor_erro(c, "WHERE: %s", c->avaliador.mensagem);

        if (permanece) {
            c->linhas_emitidas++;
            return 1;
        }
        c->linhas_descartadas++;
    }
}

/* Fecha o operador (libera pins). Idempotente. Retorna 0 ou -1. */
int plano_fechar(CursorPlano *c)
{
    int status = 0;

    if (c == NULL)
        return -1;
    if (c->aberto) {
        status = (c->metodo == ACESSO_INDEX_SCAN) ? index_scan_fechar(&c->index_scan)
                                                  : table_scan_fechar(&c->table_scan);
        c->aberto = 0;
    }
    return status;
}

/* ------------------------------------------------------------------ */
/* Exibicao do plano (equivalente a um EXPLAIN)                        */
/* ------------------------------------------------------------------ */

static void plano_imprimir_limite(int64_t v, int ilimitado, const char *simbolo)
{
    if (ilimitado) printf("%s", simbolo);
    else           printf("%lld", (long long)v);
}

static void plano_imprimir(const PlanoExecucao *p)
{
    printf("  Plano: %s", p->metodo == ACESSO_INDEX_SCAN ? "Index Scan" : "Table Scan");
    if (p->metodo == ACESSO_INDEX_SCAN)
        printf(" em %s (coluna '%s')", p->indice.indice.nome,
               p->tabela->colunas[p->indice.indice.coluna].nome);
    printf("\n         motivo: %s\n", plano_texto_motivo(p->motivo));

    if (p->indice.usa_indice && !p->indice.vazio) {
        printf("         faixa da chave: [");
        plano_imprimir_limite(p->indice.inferior, p->indice.inferior == (int64_t)INT32_MIN, "-inf");
        printf(", ");
        plano_imprimir_limite(p->indice.superior, p->indice.superior == (int64_t)INT32_MAX, "+inf");
        printf("]  (%d condicao(oes) no indice, %d residual(is))\n",
               p->indice.condicoes_no_indice, p->indice.condicoes_residuais);
    }
    if (p->custos_calculados) {
        printf("         estatisticas: altura=%d, folhas~%d, linhas~%.0f, chaves [%d, %d]\n",
               p->stats.altura, p->stats.folhas, p->stats.linhas,
               (int)p->stats.chave_min, (int)p->stats.chave_max);
        printf("         faixa: ~%.0f linha(s) (%.0f%% da tabela)\n",
               p->linhas_faixa, p->selectividade * 100.0);
        printf("         custo estimado: Table Scan = %.2f | Index Scan = %.2f\n",
               p->custo_table_scan, p->custo_index_scan);
    }
}

/* ------------------------------------------------------------------ */
/* Demonstracao (compile com -DEXECUTION_PLAN_MAIN)                    */
/* ------------------------------------------------------------------ */

#ifdef EXECUTION_PLAN_MAIN

#define DEMO_MAX_IDS 128

typedef struct { int32_t ids[DEMO_MAX_IDS]; int n; } IdsPlanoDemo;

/* Executa o plano ate o fim, guardando os ids. Devolve as leituras fisicas de
   disco (cache frio: plano_abrir reinicia o cache) ou -1 em erro. */
static long demo_executar(Catalogo *cat, const PlanoExecucao *plano, IdsPlanoDemo *saida)
{
    CursorPlano c;
    Row         linha;
    int         r;
    long        antes = (long)contador_leituras_disco;

    saida->n = 0;
    if (plano_abrir(&c, cat, plano) != 0) {
        printf("  ERRO ao abrir: %s\n", c.mensagem);
        return -1;
    }
    while ((r = plano_next(&c, &linha)) == 1) {
        int32_t id = 0;
        row_obter_int(&linha, 0, &id);
        if (saida->n < DEMO_MAX_IDS)
            saida->ids[saida->n] = id;
        saida->n++;
    }
    if (r < 0)
        printf("  ERRO durante a execucao: %s\n", c.mensagem);
    plano_fechar(&c);
    return r < 0 ? -1 : (long)contador_leituras_disco - antes;
}

static void demo_consulta(Catalogo *cat, const char *sql)
{
    Parser        parser;
    Comando       cmd;
    PlanoExecucao plano, alternativo;
    IdsPlanoDemo  escolhido, outro;

    printf("\n>> %s\n", sql);
    iniciar_parser(&parser, sql);
    if (!parse_comando(&parser, &cmd)) {
        printf("  erro de sintaxe: %s\n", parser.mensagem);
        return;
    }
    if (plano_criar(&plano, cat, &cmd) != 0) {
        printf("  ERRO ao planejar: %s\n", plano.mensagem);
        return;
    }
    plano_imprimir(&plano);

    long leituras = demo_executar(cat, &plano, &escolhido);
    if (leituras < 0)
        return;
    printf("  resultado: %d linha(s) | %ld leitura(s) fisica(s) com o metodo escolhido\n",
           escolhido.n, leituras);

    /* Conferencia: o outro metodo deve entregar exatamente as mesmas linhas. */
    alternativo = plano;
    alternativo.metodo = (plano.metodo == ACESSO_INDEX_SCAN) ? ACESSO_TABLE_SCAN : ACESSO_INDEX_SCAN;
    if (alternativo.metodo == ACESSO_INDEX_SCAN && !plano.indice.usa_indice) {
        printf("  (sem metodo alternativo: o Index Scan nao se aplica)\n");
        return;
    }
    long leituras_alt = demo_executar(cat, &alternativo, &outro);
    if (leituras_alt < 0)
        return;
    int n_cmp = escolhido.n < DEMO_MAX_IDS ? escolhido.n : DEMO_MAX_IDS;
    int igual = escolhido.n == outro.n &&
                memcmp(escolhido.ids, outro.ids, sizeof(int32_t) * (size_t)n_cmp) == 0;
    printf("  alternativa (%s): %ld leitura(s) fisica(s) -> resultado %s\n",
           alternativo.metodo == ACESSO_INDEX_SCAN ? "Index Scan" : "Table Scan",
           leituras_alt, igual ? "identico (esperado)" : "DIVERGENTE");
}

int main(void)
{
    static Catalogo cat;
    Coluna colunas[2];
    Row    linha;

    remove("alunos.dat");               /* parte de um arquivo de dados limpo */
    catalogo_iniciar(&cat);

    strcpy(colunas[0].nome, "id");   colunas[0].tipo = TOKEN_INT;
    strcpy(colunas[1].nome, "nome"); colunas[1].tipo = TOKEN_CHAR;
    if (!catalogo_adicionar_tabela(&cat, "alunos", colunas, 2)) {
        fprintf(stderr, "Erro no catalogo: %s\n", cat.mensagem);
        return 1;
    }
    const TabelaCat *alunos = catalogo_buscar_tabela(&cat, "alunos");

    /* 60 linhas, ids 1..60 inseridos fora de ordem (37 e' primo com 60) */
    const int quantidade = 60;
    for (int i = 0; i < quantidade; i++) {
        char nome[TAM_CHAR + 1];
        snprintf(nome, sizeof nome, "n%u", (unsigned)i % 1000u);
        row_iniciar(&linha, alunos);
        row_definir_int(&linha, 0, (int32_t)((i * 37) % quantidade + 1));
        row_definir_char(&linha, 1, nome);
        if (row_inserir(&cat, &linha) != INSERE_OK) {
            fprintf(stderr, "Falha ao inserir a linha %d\n", i);
            return 1;
        }
    }
    printf("%d linhas inseridas em 'alunos' (fora de ordem).\n", quantidade);

    demo_consulta(&cat, "SELECT * FROM alunos WHERE id = 30;");                    /* pontual          */
    demo_consulta(&cat, "SELECT * FROM alunos WHERE id >= 10 AND id < 20;");       /* faixa estreita   */
    demo_consulta(&cat, "SELECT * FROM alunos WHERE id >= 30 AND nome <> 'n31';"); /* faixa + residual */
    demo_consulta(&cat, "SELECT * FROM alunos WHERE id >= 2;");                    /* faixa larga      */
    demo_consulta(&cat, "SELECT * FROM alunos WHERE id > 1000;");                  /* fora do dominio  */
    demo_consulta(&cat, "SELECT * FROM alunos WHERE id > 50 AND id < 20;");        /* contradicao      */
    demo_consulta(&cat, "SELECT * FROM alunos WHERE id <> 30;");                   /* nao indexavel    */
    demo_consulta(&cat, "SELECT * FROM alunos WHERE nome = 'n5';");                /* sem indice util  */
    demo_consulta(&cat, "SELECT * FROM alunos;");                                  /* sem WHERE        */
    demo_consulta(&cat, "SELECT * FROM fantasma WHERE id = 1;");                   /* tabela inexistente */

    printf("\nreiniciar_cache() apos as consultas -> %s\n",
           reiniciar_cache() == 0 ? "aceito (nenhum pin vazado)" : "recusado (pin vazado!)");

    remove("alunos.dat");
    return 0;
}

#endif /* EXECUTION_PLAN_MAIN */

#endif /* EXECUTION_PLAN */
