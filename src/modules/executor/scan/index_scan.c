/* ============================================================
 *                 INDEX SCAN (VARREDURA POR INDICE)
 * ============================================================
 *
 * Operador que responde a um SELECT ... WHERE usando a arvore B+ em vez de
 * varrer a tabela inteira. Segue o modelo iterador dos demais operadores:
 *
 *      index_scan_abrir()  ->  index_scan_next() ... index_scan_next()  ->  index_scan_fechar()
 *
 * Pipeline (cada etapa e' uma funcao separada):
 *
 *   1) CATALOGO     catalogo_listar_indices()   quais indices existem na tabela
 *   2) B+           bplus_verificar_operacoes() o que a arvore sabe fazer
 *   3) PREDICADOS   planejar_index_scan()       quais condicoes do WHERE viram
 *                                               limites de busca no indice
 *   4) INDICE       index_scan_proxima_ref()    desce ate a folha do limite
 *                                               inferior e anda pela cadeia de
 *                                               folhas, parando no limite superior
 *   5) REFERENCIA   RefRegistro (pagina, slot)  resultado do indice = endereco
 *                                               do registro, nao o registro
 *   6) REGISTRO     buscar_registro_por_ref()   le a pagina via cache (pin) e
 *                                               desserializa o registro fisico
 *   7) TUPLA        row_de_registro()           Registro de 8 bytes -> row logica
 *   8) REAVALIACAO  linha_permanece_where()     o WHERE ORIGINAL COMPLETO e'
 *                                               reavaliado sobre a row
 *   9) EMISSAO      so as Rows que passam saem de index_scan_next()
 *
 * Por que reavaliar o WHERE inteiro (etapa 8)?
 *   - condicoes que o indice nao entende (<>, colunas sem indice, CHAR) sao
 *     "residuais": o indice apenas reduz o conjunto de candidatos;
 *   - mesmo as condicoes usadas no indice sao reavaliadas: o indice e' uma
 *     otimizacao, nunca a fonte da verdade sobre o que satisfaz o WHERE.
 *
 * Indices no catalogo:
 *   catalogo_listar_indices() consulta o Catalogo e reune duas fontes:
 *     a) o indice implicito "pk_<tabela>": a propria arvore B+ da tabela,
 *        chaveada pela 1a coluna (INT, unica, ordenada) - o catalogo exige isso
 *        em catalogo_adicionar_tabela(). Suas folhas guardam o registro inteiro;
 *     b) os indices criados por CREATE INDEX (cat->indices), cada um com a sua
 *        arvore em um arquivo ".idx" proprio.
 *   Todos aparecem na lista, mas so os marcados como 'executavel' entram no
 *   plano. Hoje isso vale apenas para o "pk_": o INSERT do catalogo grava so na
 *   arvore da tabela, entao as arvores ".idx" ficam vazias e o formato de suas
 *   folhas (chave -> referencia) nao esta definido. Usa-las devolveria
 *   resultados incompletos. Quando o INSERT passar a manter os indices
 *   secundarios, basta marcar 'executavel' em catalogo_listar_indices() e
 *   ensinar index_scan_posicionar()/index_scan_proxima_ref() a abrir o .idx.
 *
 * Operacoes da B+ (BPlusTree.c):
 *   publicas : buscar_valor(chave)         -> devolve o Registro, sem dizer onde esta'
 *              percorrer_registros(cb,arg) -> varredura ordenada completa com parada antecipada
 *   internas : ler_raiz / ler_no / indice_filho (visiveis aqui: mesma unidade de compilacao)
 *              e o encadeamento de folhas (campo 'proxima')
 *   Nao ha busca por faixa pronta, nem API que devolva (pagina, slot). Esta unidade
 *   monta a faixa sobre as primitivas internas: desce ate a folha do limite inferior
 *   e segue a cadeia ate passar do limite superior.
 *
 * Predicados indexaveis (SARGable):
 *   coluna_indexada  {=, <, <=, >, >=}  literal_inteiro
 *   Todas as condicoes do WHERE sao ligadas por AND, entao as indexaveis se
 *   combinam em UMA faixa fechada [inferior, superior] (em int64, sem overflow).
 *   '<>' nao e' indexavel (nao e' uma faixa continua). Faixa vazia (ex.: id > 90
 *   AND id < 20) encerra a consulta sem tocar no disco.
 *
 * Politica de pins: nenhuma pagina fica fixada entre chamadas. A folha corrente
 * e' COPIADA (ler_no) e a pagina de cada registro e' fixada so durante a leitura.
 * Assim o cache pode ser reiniciado a qualquer momento.
 *
 * Limitacao herdada do Pager: ha um unico arquivo corrente. Use UMA varredura
 * ativa por vez e nao acesse outra tabela entre abrir e fechar.
 *
 * Retorno:
 *   index_scan_abrir   : 0 = aberta, 1 = indice nao aplicavel (use table_scan), -1 = erro
 *   index_scan_next    : 1 = tupla emitida, 0 = fim, -1 = erro
 *   index_scan_fechar  : 0 = sucesso, -1 = erro
 */

#ifndef INDEX_SCAN
#define INDEX_SCAN

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

/* Cada arquivo abaixo protege o proprio main (ver table_scan.c / predicate_evaluator.c). */
#include "table_scan.c"            /* traz row, catalogo, B+, cache e page */
#include "../evaluator/predicate_evaluator.c"   /* linha_permanece_where() */

/* pk implicito + no maximo um CREATE INDEX por coluna (o catalogo recusa repetir) */
#define MAX_INDICES_TABELA      (MAX_COLUNAS + 1)
#define INDEX_SCAN_NAO_APLICAVEL 1

/* Offsets do cabecalho de folha (espelham serializar_no) */
#define IDX_OFFSET_TIPO          0
#define IDX_OFFSET_NUM_CHAVES    4

/* ------------------------------------------------------------------ */
/* Etapa 1: indices disponiveis no catalogo                            */
/* ------------------------------------------------------------------ */

/* Visao que o Index Scan tem de um indice do catalogo. (IndiceCat, sem sufixo,
   e' o registro de metadados do proprio Catalog.c.) */
typedef struct {
    char      nome[MAX_LEXEMA + 8];  /* ex.: "pk_alunos" ou o nome do CREATE INDEX */
    char      arquivo[TAM_ARQUIVO];  /* arquivo da arvore (.dat da tabela ou .idx)  */
    int       coluna;                /* posicao da coluna indexada na tabela        */
    TipoToken tipo;                  /* tipo da chave (so TOKEN_INT e' suportado)   */
    int       unico;                 /* 1 = sem chaves repetidas                    */
    int       ordenado;              /* 1 = folhas em ordem crescente da chave      */
    int       clusterizado;          /* 1 = a folha guarda o proprio registro       */
    int       executavel;            /* 1 = o Index Scan sabe percorrer este indice */
} IndiceDisponivel;

typedef struct {
    IndiceDisponivel itens[MAX_INDICES_TABELA];
    int              quantidade;
} ListaIndices;

/*
 catalogo_listar_indices(cat, tabela, lista)
 Consulta o catalogo e preenche a lista com TODOS os indices da tabela: o "pk_"
 implicito (arvore da propria tabela) e os registrados por CREATE INDEX.
 Nao toca no disco nem no Pager: so le metadados. Quais podem ser usados no
 plano e' dito por 'executavel' (ver "Indices no catalogo" no cabecalho).
 Retorna a quantidade listada (0 = a tabela nao tem indice).
*/
static int catalogo_listar_indices(const Catalogo *cat, const TabelaCat *tabela,
                                   ListaIndices *lista) {
    memset(lista, 0, sizeof(*lista));
    if (cat == NULL || tabela == NULL) return 0;

    /* a) A arvore B+ da tabela e' chaveada pela 1a coluna, que deve ser INT. */
    if (tabela->numero_colunas >= 1 && tabela->colunas[0].tipo == TOKEN_INT) {
        IndiceDisponivel *ix = &lista->itens[lista->quantidade++];
        snprintf(ix->nome, sizeof(ix->nome), "pk_%s", tabela->nome);
        snprintf(ix->arquivo, sizeof(ix->arquivo), "%s", tabela->arquivo);
        ix->coluna       = 0;
        ix->tipo         = TOKEN_INT;
        ix->unico        = 1;
        ix->ordenado     = 1;
        ix->clusterizado = 1;
        ix->executavel   = 1;
    }

    /* b) Indices registrados por CREATE INDEX nesta tabela. */
    for (int i = 0; i < cat->numero_indices; i++) {
        const IndiceCat *reg = &cat->indices[i];
        if (!contexto_igual(reg->tabela, tabela->nome)) continue;

        /* Metadado incoerente com a tabela: ignora em vez de indexar fora do vetor. */
        if (reg->indice_coluna < 0 || reg->indice_coluna >= tabela->numero_colunas) {
            fprintf(stderr, "Aviso: indice '%s' aponta para coluna inexistente (%d); ignorado\n",
                    reg->nome, reg->indice_coluna);
            continue;
        }
        if (lista->quantidade >= MAX_INDICES_TABELA) {
            fprintf(stderr, "Aviso: tabela '%s' tem mais indices que o suportado (%d); excedentes ignorados\n",
                    tabela->nome, MAX_INDICES_TABELA);
            break;
        }

        IndiceDisponivel *ix = &lista->itens[lista->quantidade++];
        snprintf(ix->nome, sizeof(ix->nome), "%s", reg->nome);
        snprintf(ix->arquivo, sizeof(ix->arquivo), "%s", reg->arquivo);
        ix->coluna       = reg->indice_coluna;
        ix->tipo         = reg->tipo_chave;
        ix->unico        = 0;      /* o catalogo nao registra unicidade */
        ix->ordenado     = 1;      /* e' uma arvore B+ */
        ix->clusterizado = 0;      /* a folha nao guarda o registro da tabela */
        ix->executavel   = 0;      /* arvore .idx nao e' populada pelo INSERT (ver cabecalho) */
    }
    return lista->quantidade;
}

static void imprimir_indices_disponiveis(const TabelaCat *tabela, const ListaIndices *lista) {
    printf("  Catalogo: %d indice(s) em '%s'\n", lista->quantidade, tabela->nome);
    for (int i = 0; i < lista->quantidade; i++) {
        const IndiceDisponivel *ix = &lista->itens[i];
        printf("         %s (coluna '%s', arquivo %s) -> %s\n", ix->nome,
               tabela->colunas[ix->coluna].nome, ix->arquivo,
               ix->executavel ? "usavel pelo Index Scan"
                              : "listado, nao usado (arvore secundaria nao e' mantida pelo INSERT)");
    }
}

/* ------------------------------------------------------------------ */
/* Etapa 2: operacoes oferecidas pela arvore B+                        */
/* ------------------------------------------------------------------ */

typedef struct {
    int busca_pontual;       /* buscar_valor(): devolve o Registro, mas nao a posicao */
    int percurso_ordenado;   /* percorrer_registros() e cadeia de folhas em ordem     */
    int parada_antecipada;   /* o percurso pode parar antes do fim                    */
    int posicionamento;      /* descer da raiz ate a folha de uma chave (ler_raiz +
                                ler_no + indice_filho)                                */
    int folhas_encadeadas;   /* campo 'proxima' liga as folhas                        */
} BPlusCapacidades;

/*
 bplus_verificar_operacoes(cap)
 As capacidades decorrem do que BPlusTree.c implementa (declaradas aqui, num so
 lugar). Em tempo de execucao, confere ainda que a arvore do arquivo corrente
 e' legivel (raiz valida e no com tipo conhecido); sem isso nenhuma operacao
 pode ser prometida. Retorna 0 em sucesso, -1 em erro.
*/
static int bplus_verificar_operacoes(BPlusCapacidades *cap) {
    memset(cap, 0, sizeof(*cap));

    int raiz = ler_raiz();
    int total = total_paginas_alocadas();
    if (raiz <= PAGINA_METADADOS || raiz >= total) {
        fprintf(stderr, "Erro: raiz da arvore B+ invalida (%d)\n", raiz);
        return -1;
    }
    No no;
    if (ler_no(raiz, &no) != 0) return -1;
    if (no.tipo != NO_FOLHA && no.tipo != NO_INTERNO) {
        fprintf(stderr, "Erro: no raiz da arvore B+ com tipo desconhecido (%d)\n", (int)no.tipo);
        return -1;
    }

    cap->busca_pontual     = 1;
    cap->percurso_ordenado = 1;
    cap->parada_antecipada = 1;
    cap->posicionamento    = 1;
    cap->folhas_encadeadas = 1;
    return 0;
}

static void imprimir_capacidades(const BPlusCapacidades *cap) {
    printf("  Operacoes da B+: busca pontual=%s, percurso ordenado=%s, parada antecipada=%s,\n"
           "                   posicionamento por chave=%s, folhas encadeadas=%s\n"
           "                   (faixa: montada aqui sobre posicionamento + folhas encadeadas)\n",
           cap->busca_pontual ? "sim" : "nao", cap->percurso_ordenado ? "sim" : "nao",
           cap->parada_antecipada ? "sim" : "nao", cap->posicionamento ? "sim" : "nao",
           cap->folhas_encadeadas ? "sim" : "nao");
}

/* ------------------------------------------------------------------ */
/* Etapa 3: predicados que podem usar o indice                         */
/* ------------------------------------------------------------------ */

typedef struct {
    int       usa_indice;           /* 1 = ha ao menos um predicado indexavel          */
    int       vazio;                /* 1 = faixa contraditoria: nenhum registro casa   */
    int       pontual;              /* 1 = inferior == superior (igualdade)            */
    IndiceDisponivel indice;        /* indice escolhido                                */
    int64_t   inferior;             /* limite inferior INCLUSIVO da chave              */
    int64_t   superior;             /* limite superior INCLUSIVO da chave              */
    int       condicoes_no_indice;  /* condicoes convertidas em limites                */
    int       condicoes_residuais;  /* condicoes que so a reavaliacao trata            */
    unsigned long mascara;          /* bit i = condicao i do WHERE foi usada no indice */
} PlanoIndexScan;

/* A condicao (coluna op literal) pode virar limite de busca neste indice? */
static int condicao_usavel_no_indice(const TabelaCat *tabela, const IndiceDisponivel *ix,
                                     const Condicao *c, const BPlusCapacidades *cap) {
    if (tabela_buscar_coluna(tabela, c->coluna) != ix->coluna) return 0;
    if (ix->tipo != TOKEN_INT || c->valor.tipo != TOKEN_NUMERO) return 0;

    switch (c->operacao) {
        case TOKEN_IGUAL:
            return cap->posicionamento;
        case TOKEN_MENOR:
        case TOKEN_MENOR_IGUAL:
        case TOKEN_MAIOR:
        case TOKEN_MAIOR_IGUAL:
            return cap->posicionamento && cap->folhas_encadeadas && cap->percurso_ordenado;
        default:                       /* '<>' nao e' uma faixa continua */
            return 0;
    }
}

/* Estreita a faixa [inferior, superior]. int64 evita overflow em v+1 / v-1. */
static void aplicar_limite(PlanoIndexScan *p, TipoToken op, int64_t v) {
    switch (op) {
        case TOKEN_IGUAL:
            if (v > p->inferior) p->inferior = v;
            if (v < p->superior) p->superior = v;
            break;
        case TOKEN_MAIOR:
            if (v + 1 > p->inferior) p->inferior = v + 1;
            break;
        case TOKEN_MAIOR_IGUAL:
            if (v > p->inferior) p->inferior = v;
            break;
        case TOKEN_MENOR:
            if (v - 1 < p->superior) p->superior = v - 1;
            break;
        case TOKEN_MENOR_IGUAL:
            if (v < p->superior) p->superior = v;
            break;
        default:
            break;
    }
}

/* Plano de um unico indice: combina todas as condicoes indexaveis numa faixa. */
static void planejar_com_indice(const TabelaCat *tabela, const Comando *cmd,
                                const IndiceDisponivel *ix, const BPlusCapacidades *cap,
                                PlanoIndexScan *p) {
    memset(p, 0, sizeof(*p));
    p->indice   = *ix;
    p->inferior = (int64_t)INT32_MIN;
    p->superior = (int64_t)INT32_MAX;

    for (int i = 0; i < cmd->numero_condicoes; i++) {
        const Condicao *c = &cmd->condicoes[i];
        if (!condicao_usavel_no_indice(tabela, ix, c, cap)) continue;

        aplicar_limite(p, c->operacao, (int64_t)c->valor.numero);
        p->condicoes_no_indice++;
        if (i < (int)(8 * sizeof(p->mascara))) p->mascara |= 1UL << i;
    }

    p->condicoes_residuais = cmd->numero_condicoes - p->condicoes_no_indice;
    if (p->condicoes_no_indice > 0) {
        p->usa_indice = 1;
        p->vazio      = p->inferior > p->superior;
        p->pontual    = !p->vazio && p->inferior == p->superior;
    }
}

/*
 planejar_index_scan(...)
 Avalia cada indice EXECUTAVEL da lista e escolhe o que absorve mais condicoes
 do WHERE. Indices listados pelo catalogo mas nao executaveis sao pulados.
 Se nenhum predicado for indexavel, plano->usa_indice = 0.
*/
static void planejar_index_scan(const TabelaCat *tabela, const Comando *cmd,
                                const ListaIndices *indices, const BPlusCapacidades *cap,
                                PlanoIndexScan *melhor) {
    memset(melhor, 0, sizeof(*melhor));
    for (int i = 0; i < indices->quantidade; i++) {
        if (!indices->itens[i].executavel) continue;
        PlanoIndexScan candidato;
        planejar_com_indice(tabela, cmd, &indices->itens[i], cap, &candidato);
        if (candidato.usa_indice &&
            (!melhor->usa_indice || candidato.condicoes_no_indice > melhor->condicoes_no_indice)) {
            *melhor = candidato;
        }
    }
}

static void index_scan_imprimir_plano(const TabelaCat *tabela, const Comando *cmd,
                                      const PlanoIndexScan *p) {
    if (!p->usa_indice) {
        printf("  Plano: nenhum predicado indexavel -> Index Scan nao se aplica (usar Table Scan)\n");
        return;
    }
    printf("  Plano: Index Scan em %s (coluna '%s'), %s\n", p->indice.nome,
           tabela->colunas[p->indice.coluna].nome,
           p->vazio ? "faixa vazia (contradicao)" : p->pontual ? "busca pontual" : "busca por faixa");
    if (!p->vazio) {
        printf("         faixa da chave: [");
        if (p->inferior == (int64_t)INT32_MIN) printf("-inf");
        else                                   printf("%lld", (long long)p->inferior);
        printf(", ");
        if (p->superior == (int64_t)INT32_MAX) printf("+inf");
        else                                   printf("%lld", (long long)p->superior);
        printf("]\n");
    }
    for (int i = 0; i < cmd->numero_condicoes && i < (int)(8 * sizeof(p->mascara)); i++) {
        const Condicao *c = &cmd->condicoes[i];
        printf("         condicao %d: %s %s ", i + 1, c->coluna, nome_token(c->operacao));
        if (c->valor.tipo == TOKEN_NUMERO) printf("%d", c->valor.numero);
        else                               printf("'%s'", c->valor.str);
        printf("  -> %s\n", (p->mascara >> i) & 1UL ? "usada no indice" : "residual (so reavaliacao)");
    }
}

/* ------------------------------------------------------------------ */
/* Etapa 5: referencia a um registro                                   */
/* ------------------------------------------------------------------ */

/* Endereco de um registro: pagina da folha + slot. A chave acompanha a
   referencia para detectar referencia velha (o slot passou a ter outro id). */
typedef struct {
    int32_t pagina;
    int     slot;
    int32_t chave;
} RefRegistro;

/* ------------------------------------------------------------------ */
/* Estado da varredura                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    const TabelaCat *tabela;
    const Comando   *comando;        /* WHERE original (reavaliado por inteiro)     */
    PlanoIndexScan   plano;
    Avaliador        avaliador;
    int              aberta;
    int              terminada;

    No               folha;          /* COPIA da folha corrente (sem pin)           */
    int              pagina_folha;
    int              proximo_slot;
    int              folhas_visitadas;   /* protecao contra ciclo na cadeia         */

    /* estatisticas */
    long refs_geradas;               /* etapa 5: referencias produzidas pelo indice */
    long registros_lidos;            /* etapa 6: leituras de registro por referencia*/
    long descartadas;                /* etapa 8: reprovadas pela reavaliacao        */
    long emitidas;                   /* etapa 9: tuplas entregues                   */
} IndexScan;

static int index_scan_falhar(IndexScan *s) {
    s->terminada = 1;
    return -1;
}

/* ------------------------------------------------------------------ */
/* Etapa 4: percorrer o indice                                         */
/* ------------------------------------------------------------------ */

/* Copia a folha 'pagina' para scan->folha, validando limites e tipo. */
static int index_scan_carregar_folha(IndexScan *s, int pagina) {
    int total = total_paginas_alocadas();
    if (++s->folhas_visitadas > total || pagina <= PAGINA_METADADOS || pagina >= total) {
        fprintf(stderr, "Erro: cadeia de folhas de '%s' corrompida (pagina %d)\n",
                s->tabela->nome, pagina);
        return -1;
    }
    if (ler_no(pagina, &s->folha) != 0) return -1;
    if (s->folha.tipo != NO_FOLHA) {
        fprintf(stderr, "Erro: pagina %d de '%s' nao e' folha (tipo=%d)\n",
                pagina, s->tabela->nome, (int)s->folha.tipo);
        return -1;
    }
    s->pagina_folha = pagina;
    s->proximo_slot = 0;
    return 0;
}

/* Desce da raiz ate a folha onde a chave 'chave' esta' (ou estaria). */
static int index_scan_posicionar(IndexScan *s, int32_t chave) {
    int pagina = ler_raiz();
    int limite = total_paginas_alocadas();
    No  no;

    for (int passos = 0; ; passos++) {
        if (pagina < 0 || passos > limite || pagina <= PAGINA_METADADOS || pagina >= limite) {
            fprintf(stderr, "Erro: arvore de '%s' corrompida (pagina %d invalida na descida)\n",
                    s->tabela->nome, pagina);
            return -1;
        }
        if (ler_no(pagina, &no) != 0) return -1;
        if (no.tipo == NO_FOLHA) break;
        pagina = no.filhos[indice_filho(&no, chave)];
    }

    s->folha            = no;
    s->pagina_folha     = pagina;
    s->proximo_slot     = 0;
    s->folhas_visitadas = 1;
    return 0;
}

/*
 index_scan_proxima_ref(scan, ref)
 Proxima referencia cuja chave esta' em [inferior, superior].
 As folhas estao em ordem crescente: ao ver uma chave acima do limite superior
 nao ha mais nada a procurar (parada antecipada).
 Retorna 1 (ref preenchida), 0 (fim) ou -1 (erro).
*/
static int index_scan_proxima_ref(IndexScan *s, RefRegistro *ref) {
    while (!s->terminada) {
        if (s->proximo_slot >= s->folha.numero_chaves) {
            if (s->folha.proxima == SEM_PAGINA) {
                s->terminada = 1;
                break;
            }
            if (index_scan_carregar_folha(s, s->folha.proxima) != 0) return -1;
            continue;
        }

        int     slot  = s->proximo_slot++;
        int32_t chave = s->folha.registros[slot].id;

        if (chave < s->plano.inferior) continue;      /* ainda antes da faixa */
        if (chave > s->plano.superior) {              /* passou da faixa      */
            s->terminada = 1;
            break;
        }

        ref->pagina = s->pagina_folha;
        ref->slot   = slot;
        ref->chave  = chave;
        s->refs_geradas++;
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Etapa 6: referencia -> Registro fisico                              */
/* ------------------------------------------------------------------ */

/*
 buscar_registro_por_ref(ref, reg)
 Fixa a pagina da referencia no cache, desserializa o slot e solta a pagina
 (somente leitura: nunca suja). Confere tipo da pagina, slot e chave.
 Retorna 0 em sucesso, -1 em erro.
*/
static int buscar_registro_por_ref(const RefRegistro *ref, Registro *reg) {
    unsigned char *pagina = fixar(ref->pagina);
    if (pagina == NULL) {
        fprintf(stderr, "Erro: nao foi possivel abrir a pagina %d da referencia\n", (int)ref->pagina);
        return -1;
    }

    int32_t  tipo     = (int32_t)desserializar_uint32_little_endian(pagina + IDX_OFFSET_TIPO);
    uint32_t ocupados = desserializar_uint32_little_endian(pagina + IDX_OFFSET_NUM_CHAVES);
    if (tipo != NO_FOLHA || ocupados > (uint32_t)MAX_CHAVES ||
        ref->slot < 0 || (uint32_t)ref->slot >= ocupados) {
        fprintf(stderr, "Erro: referencia invalida (pagina %d, slot %d)\n", (int)ref->pagina, ref->slot);
        soltar(ref->pagina, 0);
        return -1;
    }

    desserializar_registro(pagina + OFFSET_REGISTROS + (size_t)ref->slot * TAMANHO_REGISTRO, reg);
    soltar(ref->pagina, 0);

    if (reg->id != ref->chave) {
        fprintf(stderr, "Erro: referencia desatualizada (pagina %d, slot %d: esperado id=%d, lido id=%d)\n",
                (int)ref->pagina, ref->slot, (int)ref->chave, (int)reg->id);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Interface publica                                                   */
/* ------------------------------------------------------------------ */

/*
 index_scan_abrir(scan, cat, cmd)
 Valida o SELECT contra o catalogo, descobre os indices, confere as operacoes da
 B+, planeja e posiciona o indice no limite inferior. O catalogo e o Comando
 devem continuar vivos ate index_scan_fechar().
 Retorna 0 (aberta), INDEX_SCAN_NAO_APLICAVEL=1 (nenhum predicado usa indice:
 nada foi aberto, use table_scan) ou -1 (erro).
*/
int index_scan_abrir(IndexScan *scan, Catalogo *cat, const Comando *cmd) {
    if (scan == NULL || cat == NULL || cmd == NULL) {
        fprintf(stderr, "Erro: index_scan_abrir recebeu argumento NULL\n");
        return -1;
    }
    memset(scan, 0, sizeof(*scan));
    scan->terminada = 1;

    if (cmd->tipo != CMD_SELECT) {
        fprintf(stderr, "Erro: index_scan_abrir exige um comando SELECT\n");
        return -1;
    }

    /* Catalogo: tabela existe, colunas do WHERE existem, tipos dos literais conferem */
    const TabelaCat *tabela = catalogo_buscar_tabela(cat, cmd->tabela);
    if (tabela == NULL) {
        fprintf(stderr, "Erro: tabela '%s' nao existe no catalogo\n", cmd->tabela);
        return -1;
    }
    if (!catalogo_validar_select(cat, cmd)) {
        fprintf(stderr, "Erro: %s\n", cat->mensagem);
        return -1;
    }

    /* 1) indices disponiveis (so metadados: pode ser feito antes de trocar de arquivo) */
    ListaIndices indices;
    catalogo_listar_indices(cat, tabela, &indices);

    /* O Pager precisa apontar para o arquivo da tabela antes de qualquer leitura */
    if (!selecionar_arquivo_tabela(cat, tabela)) {
        fprintf(stderr, "Erro: %s\n", cat->mensagem);
        return -1;
    }

    /* 2) operacoes da B+  e  3) predicados indexaveis */
    BPlusCapacidades cap;
    if (bplus_verificar_operacoes(&cap) != 0) return -1;

    scan->tabela  = tabela;
    scan->comando = cmd;
    planejar_index_scan(tabela, cmd, &indices, &cap, &scan->plano);

    if (!scan->plano.usa_indice) return INDEX_SCAN_NAO_APLICAVEL;

    iniciar_avaliador(&scan->avaliador);
    scan->aberta = 1;

    if (scan->plano.vazio) return 0;     /* contradicao: terminada=1, sem I/O no indice */

    /* 4) posiciona na folha do limite inferior */
    if (index_scan_posicionar(scan, (int32_t)scan->plano.inferior) != 0) {
        scan->aberta = 0;
        return -1;
    }
    scan->terminada = 0;
    return 0;
}

/*
 index_scan_next(scan, saida)
 Entrega a proxima row que satisfaz o WHERE completo, em ordem crescente da chave.
 Retorna 1 (row em *saida), 0 (fim) ou -1 (erro).
*/
int index_scan_next(IndexScan *scan, Row *saida) {
    if (scan == NULL || saida == NULL) {
        fprintf(stderr, "Erro: index_scan_next recebeu argumento NULL\n");
        return -1;
    }
    if (!scan->aberta) {
        fprintf(stderr, "Erro: index_scan_next chamado em varredura nao aberta\n");
        return -1;
    }

    for (;;) {
        RefRegistro ref;
        Registro    reg;
        Row         row;
        int         permanece;

        /* 4+5) indice -> referencia */
        int r = index_scan_proxima_ref(scan, &ref);
        if (r < 0) return index_scan_falhar(scan);
        if (r == 0) return 0;

        /* 6) referencia -> Registro */
        if (buscar_registro_por_ref(&ref, &reg) != 0) return index_scan_falhar(scan);
        scan->registros_lidos++;

        /* 7) Registro -> Row */
        if (row_de_registro(&row, scan->tabela, &reg) != 0) return index_scan_falhar(scan);

        /* 8) reavalia o WHERE original completo */
        if (linha_permanece_where(&scan->avaliador, scan->comando, &row, &permanece) != 0) {
            fprintf(stderr, "Erro ao reavaliar o WHERE: %s\n", scan->avaliador.mensagem);
            return index_scan_falhar(scan);
        }
        if (!permanece) {
            scan->descartadas++;
            continue;
        }

        /* 9) emite */
        *saida = row;
        scan->emitidas++;
        return 1;
    }
}

/* Encerra a varredura. Nao ha pins a liberar (ver "Politica de pins"). Idempotente. */
int index_scan_fechar(IndexScan *scan) {
    if (scan == NULL) return -1;
    scan->aberta    = 0;
    scan->terminada = 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Demonstracao                                                        */
/* ------------------------------------------------------------------ */

/* Ids emitidos, para comparar Index Scan x Table Scan. Tipo proprio: o ListaIds de
   table_scan.c so existe com -DTABLE_SCAN_MAIN, e reutilizar o nome causaria
   redefinicao quando essa macro estiver ligada. */
typedef struct { int32_t ids[64]; int n; } ListaIdsDemo;

/* Referencia: Table Scan + o mesmo WHERE. Serve para conferir o Index Scan. */
static int demo_table_scan_com_where(Catalogo *cat, const Comando *cmd, ListaIdsDemo *saida) {
    TableScan ts;
    Row       row;
    Avaliador av;
    int       r, permanece;

    iniciar_avaliador(&av);
    if (table_scan_abrir(&ts, cat, cmd->tabela) != 0) return -1;
    while ((r = table_scan_next(&ts, &row)) == 1) {
        if (linha_permanece_where(&av, cmd, &row, &permanece) != 0) {
            table_scan_fechar(&ts);
            return -1;
        }
        if (permanece) {
            int32_t id = 0;
            row_obter_int(&row, 0, &id);
            if (saida->n < 64) saida->ids[saida->n++] = id;
        }
    }
    table_scan_fechar(&ts);
    return (r < 0) ? -1 : 0;
}

static void demo_select(Catalogo *cat, const char *sql) {
    Parser    parser;
    Comando   cmd;
    IndexScan scan;
    Row       tupla;
    ListaIdsDemo via_indice = { {0}, 0 }, via_scan = { {0}, 0 };

    printf("\n>> %s\n", sql);
    iniciar_parser(&parser, sql);
    if (!parse_comando(&parser, &cmd)) {
        printf("  erro de sintaxe: %s\n", parser.mensagem);
        return;
    }

    long leituras_antes = contador_leituras_disco;
    int  r = index_scan_abrir(&scan, cat, &cmd);
    if (r < 0) {
        printf("  erro ao abrir o Index Scan\n");
        return;
    }

    const TabelaCat *tabela = catalogo_buscar_tabela(cat, cmd.tabela);
    ListaIndices     indices;
    catalogo_listar_indices(cat, tabela, &indices);
    imprimir_indices_disponiveis(tabela, &indices);
    index_scan_imprimir_plano(tabela, &cmd, &scan.plano);

    if (r == INDEX_SCAN_NAO_APLICAVEL) {
        printf("  (o Index Scan devolveu 'nao aplicavel'; o otimizador usaria o Table Scan)\n");
        return;
    }

    int n;
    while ((n = index_scan_next(&scan, &tupla)) == 1) {
        int32_t id = 0;
        printf("  ");
        row_imprimir(&tupla);
        row_obter_int(&tupla, 0, &id);
        if (via_indice.n < 64) via_indice.ids[via_indice.n++] = id;
    }
    if (n < 0) {
        printf("  erro durante o Index Scan\n");
        index_scan_fechar(&scan);
        return;
    }
    index_scan_fechar(&scan);
    long leituras_indice = contador_leituras_disco - leituras_antes;

    printf("  %ld emitida(s) | %ld referencia(s) do indice | %ld reprovada(s) na reavaliacao | "
           "%d folha(s) visitada(s) | %ld leitura(s) fisica(s)\n",
           scan.emitidas, scan.refs_geradas, scan.descartadas, scan.folhas_visitadas, leituras_indice);

    /* Conferencia com a varredura completa + mesmo WHERE */
    leituras_antes = contador_leituras_disco;
    if (selecionar_arquivo_tabela(cat, tabela) && demo_table_scan_com_where(cat, &cmd, &via_scan) == 0) {
        long leituras_scan = contador_leituras_disco - leituras_antes;
        int  igual = via_indice.n == via_scan.n &&
                     memcmp(via_indice.ids, via_scan.ids, sizeof(int32_t) * (size_t)via_scan.n) == 0;
        printf("  conferencia com Table Scan + WHERE: %s (Table Scan: %ld leitura(s) fisica(s))\n",
               igual ? "identico (esperado)" : "DIVERGENTE", leituras_scan);
    } else {
        printf("  nao foi possivel conferir com o Table Scan\n");
    }
}
/* ------------------------------------------------------------------ */
/* Programa de demonstracao (compile com -DINDEX_SCAN_MAIN)            */
/* O main so existe com essa macro: os outros modulos incluem este     */
/* arquivo e nao podem ter dois mains.                                 */
/* ------------------------------------------------------------------ */

#ifdef INDEX_SCAN_MAIN

static int demo_inserir(Catalogo *cat, const TabelaCat *tabela, int32_t id, const char *nome) {
    Row linha;
    if (row_iniciar(&linha, tabela) != 0) return -1;
    if (row_definir_int(&linha, 0, id) != 0) return -1;
    if (row_definir_char(&linha, 1, nome) != 0) return -1;
    return row_inserir(cat, &linha) == INSERE_OK ? 0 : -1;
}

int main(void) {
    static Catalogo cat;
    Coluna colunas[2];

    remove("alunos.dat");     /* parte de arquivos de dados limpos */
    remove("idx_nome.idx");
    catalogo_iniciar(&cat);

    /* 1) Catalogo: alunos (id INT, nome CHAR) + um indice secundario em 'nome' */
    strcpy(colunas[0].nome, "id");   colunas[0].tipo = TOKEN_INT;
    strcpy(colunas[1].nome, "nome"); colunas[1].tipo = TOKEN_CHAR;
    if (!catalogo_adicionar_tabela(&cat, "alunos", colunas, 2)) {
        fprintf(stderr, "Erro no catalogo: %s\n", cat.mensagem);
        return 1;
    }
    const TabelaCat *alunos = catalogo_buscar_tabela(&cat, "alunos");

    /* 2) Carga fora de ordem: com 3 chaves por folha, forca varias divisoes */
    const int32_t ids[]   = { 50, 10, 80, 30, 70, 20, 60, 40, 90, 100, 15, 25 };
    const char   *nomes[] = { "Eva", "Ana", "Iris", "Caio", "Hugo", "Bia",
                              "Gil", "Davi", "Joao", "Kai", "Lia", "Noe" };
    const int quantidade = (int)(sizeof(ids) / sizeof(ids[0]));
    for (int i = 0; i < quantidade; i++) {
        if (demo_inserir(&cat, alunos, ids[i], nomes[i]) != 0) {
            fprintf(stderr, "Falha ao inserir a linha %d\n", i);
            return 1;
        }
    }
    printf("%d linhas inseridas em 'alunos' (fora de ordem).\n", quantidade);

    if (!catalogo_criar_indice(&cat, "alunos", "idx_nome", "nome")) {
        fprintf(stderr, "Erro ao criar o indice: %s\n", cat.mensagem);
        return 1;
    }
    printf("Indice 'idx_nome' criado em alunos(nome).\n");

    /* 3) Consultas: cada uma mostra indices do catalogo, plano, resultado e
          conferencia com Table Scan + mesmo WHERE */
    const char *consultas[] = {
        "SELECT * FROM alunos WHERE id = 30;",                    /* busca pontual           */
        "SELECT * FROM alunos WHERE id >= 30 AND id < 80;",       /* faixa fechada           */
        "SELECT * FROM alunos WHERE id > 20 AND nome <> 'Eva';",  /* faixa + condicao residual */
        "SELECT * FROM alunos WHERE id > 90 AND id < 20;",        /* faixa vazia (sem I/O)   */
        "SELECT * FROM alunos WHERE id <> 30;",                   /* '<>' nao e' indexavel   */
        "SELECT * FROM alunos WHERE nome = 'Ana';",               /* so idx_nome serviria    */
        "SELECT * FROM alunos;",                                  /* sem WHERE               */
        "SELECT * FROM fantasma WHERE id = 1;"                    /* tabela inexistente      */
    };
    for (size_t i = 0; i < sizeof(consultas) / sizeof(consultas[0]); i++) {
        demo_select(&cat, consultas[i]);
    }

    printf("\n");
    imprimir_estatisticas_cache();

    remove("alunos.dat");
    remove("idx_nome.idx");
    return 0;
}

#endif /* INDEX_SCAN_MAIN */

#endif /* INDEX_SCAN */
