/* ============================================================
 *                 ARVORE B+ (1 NO = 1 PAGINA)
 * ============================================================
 *
 * Organizacao em disco:
 *   - Pagina 0: pagina de metadados. Guarda o numero da pagina RAIZ
 *     (nos 4 primeiros bytes, em little-endian).
 *   - Demais paginas: nos da arvore, um no por pagina de 4KB.
 *
 * Layout de um no (dentro dos 4KB da pagina):
 *
 *   Cabecalho (TAMANHO_CABECALHO = 16 bytes)
 *     [ 0.. 3] tipo        (NO_INTERNO ou NO_FOLHA)
 *     [ 4.. 7] numero_chaves  (quantidade de chaves/registros em uso)
 *     [ 8..11] proxima     (folha: pagina da proxima folha, ou -1; interno: nao usado)
 *     [12..15] reservado
 *
 *   NO INTERNO (somente chaves de roteamento + ponteiros para paginas):
 *     [16..27] chaves[3]   (int32 cada)
 *     [28..43] filhos[4]   (numero de pagina de cada filho, int32 cada)
 *     filhos[i] contem chaves <  chaves[i]
 *     filhos[i+1] contem chaves >= chaves[i]
 *
 *   NO FOLHA (armazena os dados):
 *     [16..39] registros[3] (8 bytes cada: id + valor, serializados
 *                            por serializar_registro). A chave e' o 'id'.
 *
 * Cada no possui no maximo MAX_CHAVES (3) chaves. Quando uma insercao
 * faria um no ter 4 chaves, o no e' DIVIDIDO (split).
 * Fusao e redistribuicao NAO sao implementadas (nao ha remocao).
 */

#ifndef B_PLUS_TREE
#define B_PLUS_TREE

#include "Pager.c"
#include "Cache.c"

#define MAX_CHAVES   3
#define NO_INTERNO   0
#define NO_FOLHA     1
#define SEM_PAGINA   (-1)
#define PAGINA_METADADOS  0

#define OFFSET_CHAVES     TAMANHO_CABECALHO
#define OFFSET_FILHOS     (OFFSET_CHAVES + MAX_CHAVES * 4)
#define OFFSET_REGISTROS  TAMANHO_CABECALHO

#define INSERE_OK          0
#define INSERE_DIVIDIU     1   /* uso interno: o no foi dividido e ha chave a promover */
#define INSERE_DUPLICADO  (-2)
#define INSERE_ERRO       (-1)

/* Representacao em memoria de um no. Possui uma posicao extra em cada vetor
   para acomodar, temporariamente, a 4a chave antes da divisao. */

typedef struct {
    int32_t  tipo;
    int32_t  numero_chaves;
    int32_t  proxima;                       /* so para folhas */
    int32_t  chaves[MAX_CHAVES + 1];        /* so para nos internos */
    int32_t  filhos[MAX_CHAVES + 2];        /* so para nos internos */
    Registro registros[MAX_CHAVES + 1];     /* so para folhas */
} No;

/* ---------- Serializacao de nos (No <-> bytes da pagina) ---------- */
static void serializar_no(const No *no, unsigned char *pagina) {
    memset(pagina, 0, TAMANHO_PAGINA);
    serializar_uint32_little_endian((uint32_t)no->tipo,       pagina);
    serializar_uint32_little_endian((uint32_t)no->numero_chaves, pagina + 4);
    serializar_uint32_little_endian((uint32_t)no->proxima,    pagina + 8);

    if (no->tipo == NO_FOLHA) {
        for (int i = 0; i < no->numero_chaves; i++) {
            serializar_registro(&no->registros[i],
                               pagina + OFFSET_REGISTROS + i * TAMANHO_REGISTRO);
        }
    } else {
        for (int i = 0; i < no->numero_chaves; i++) {
            serializar_uint32_little_endian((uint32_t)no->chaves[i], pagina + OFFSET_CHAVES + i * 4);
        }
        for (int i = 0; i <= no->numero_chaves; i++) {
            serializar_uint32_little_endian((uint32_t)no->filhos[i], pagina + OFFSET_FILHOS + i * 4);
        }
    }
}
static void desserializar_no(const unsigned char *pagina, No *no) {
    memset(no, 0, sizeof(*no));
    no->tipo       = (int32_t)desserializar_uint32_little_endian(pagina);
    no->numero_chaves = (int32_t)desserializar_uint32_little_endian(pagina + 4);
    no->proxima    = (int32_t)desserializar_uint32_little_endian(pagina + 8);

    if (no->numero_chaves < 0 || no->numero_chaves > MAX_CHAVES) {
        no->numero_chaves = 0; /* pagina corrompida/vazia: trata como no sem chaves */
    }

    if (no->tipo == NO_FOLHA) {
        for (int i = 0; i < no->numero_chaves; i++) {
            desserializar_registro(pagina + OFFSET_REGISTROS + i * TAMANHO_REGISTRO,
                                  &no->registros[i]);
        }
    } else {
        for (int i = 0; i < no->numero_chaves; i++) {
            no->chaves[i] = (int32_t)desserializar_uint32_little_endian(pagina + OFFSET_CHAVES + i * 4);
        }
        for (int i = 0; i <= no->numero_chaves; i++) {
            no->filhos[i] = (int32_t)desserializar_uint32_little_endian(pagina + OFFSET_FILHOS + i * 4);
        }
    }
}

/* ---------- Acesso a nos via cache de paginas (fixar/soltar) ---------- */

/* Le o no da pagina n: fixa a pagina, copia para 'no' e solta (sem sujar). */
static int ler_no(int n, No *no) {
    unsigned char *pagina = fixar(n);
    if (pagina == NULL) return -1;
    desserializar_no(pagina, no);
    soltar(n, 0);
    return 0;
}

/* Grava o no na pagina n: fixa a pagina, serializa e solta marcando-a suja
   (NO-FORCE: o disco so e' atualizado na expulsao ou em descarrega()). */
static int gravar_no(int n, const No *no) {
    unsigned char *pagina = fixar(n);
    if (pagina == NULL) return -1;
    serializar_no(no, pagina);
    soltar(n, 1);
    return 0;
}

/* ---------- Pagina de metadados (guarda a raiz) ---------- */

static int ler_raiz(void) {
    unsigned char *pagina = fixar(PAGINA_METADADOS);
    if (pagina == NULL) return -1;
    int raiz = (int)(int32_t)desserializar_uint32_little_endian(pagina);
    soltar(PAGINA_METADADOS, 0);
    return raiz;
}

static int gravar_raiz(int raiz) {
    unsigned char *pagina = fixar(PAGINA_METADADOS);
    if (pagina == NULL) return -1;
    serializar_uint32_little_endian((uint32_t)raiz, pagina);
    soltar(PAGINA_METADADOS, 1);
    return 0;
}

/* Cria a arvore (pagina de metadados + raiz folha vazia) se o arquivo ainda
   estiver vazio. Retorna 0 em caso de sucesso, -1 em caso de erro. */
int inicializar_arvore(void) {
    if (total_paginas_alocadas() == 0) {
        if (alocar_pagina() != PAGINA_METADADOS) return -1;

        int pagina_raiz = alocar_pagina();
        if (pagina_raiz < 0) return -1;

        No folha;
        memset(&folha, 0, sizeof(folha));
        folha.tipo    = NO_FOLHA;
        folha.proxima = SEM_PAGINA;

        if (gravar_no(pagina_raiz, &folha) != 0) return -1;
        if (gravar_raiz(pagina_raiz) != 0) return -1;
    }
    return (ler_raiz() < 0) ? -1 : 0;
}

/* ---------- Busca ---------- */

/* Dado um no interno, devolve o indice do filho por onde a chave deve descer. */
static int indice_filho(const No *no, int32_t chave) {
    int i = 0;
    while (i < no->numero_chaves && chave >= no->chaves[i]) {
        i++;
    }
    return i;
}

/*
 Busca a chave na arvore, descendo da raiz ate a folha.
 Retorna 0 se encontrou (copiando o registro para *saida, se nao for NULL),
 1 se a chave nao existe, ou -1 em caso de erro.*/
int buscar_valor(int32_t chave, Registro *saida) {
    int pagina = ler_raiz();
    if (pagina < 0) return -1;

    No no;
    for (;;) {
        if (ler_no(pagina, &no) != 0) return -1;
        if (no.tipo == NO_FOLHA) break;
        pagina = no.filhos[indice_filho(&no, chave)];
    }

    for (int i = 0; i < no.numero_chaves; i++) {
        if (no.registros[i].id == chave) {
            if (saida != NULL) *saida = no.registros[i];
            return 0;
        }
    }
    return 1;
}

/* ---------- Divisao (split) ---------- */

/*
 Divide uma FOLHA que ficou com MAX_CHAVES+1 (4) registros.
 - A folha original (pagina 'pagina') fica com os 2 primeiros registros.
 - Uma nova pagina recebe os 2 ultimos e e' encadeada logo apos a original.
 - A chave promovida e' uma COPIA da menor chave da nova folha (em B+,
   a chave continua existindo na folha).
 Retorna INSERE_DIVIDIU em caso de sucesso, ou INSERE_ERRO.*/
int dividir_folha(int pagina, No *no, int32_t *chave_promovida, int *nova_pagina) {
    int nova = alocar_pagina();
    if (nova < 0) return INSERE_ERRO;

    int total = no->numero_chaves;          /* MAX_CHAVES + 1 */
    int meio  = (total + 1) / 2;         /* 2 registros ficam na esquerda */

    No dir;
    memset(&dir, 0, sizeof(dir));
    dir.tipo       = NO_FOLHA;
    dir.numero_chaves = total - meio;
    for (int i = 0; i < dir.numero_chaves; i++) {
        dir.registros[i] = no->registros[meio + i];
    }

    dir.proxima = no->proxima;           /* mantem a lista encadeada de folhas */
    no->proxima = nova;
    no->numero_chaves = meio;

    if (gravar_no(pagina, no) != 0) return INSERE_ERRO;
    if (gravar_no(nova, &dir) != 0) return INSERE_ERRO;

    *chave_promovida = dir.registros[0].id;
    *nova_pagina = nova;
    return INSERE_DIVIDIU;
}

/*
 Divide um NO INTERNO que ficou com MAX_CHAVES+1 (4) chaves e 5 filhos.
 - O no original fica com as 2 primeiras chaves e 3 filhos.
 - A 3a chave SOBE para o pai (nao permanece em nenhum dos dois nos).
 - Uma nova pagina recebe a 4a chave e os 2 ultimos filhos.
 Retorna INSERE_DIVIDIU em caso de sucesso, ou INSERE_ERRO.*/
int dividir_interno(int pagina, No *no, int32_t *chave_promovida, int *nova_pagina) {
    int nova = alocar_pagina();
    if (nova < 0) return INSERE_ERRO;

    int total = no->numero_chaves;          /* MAX_CHAVES + 1 */
    int meio  = total / 2;               /* indice da chave que sobe */

    No dir;
    memset(&dir, 0, sizeof(dir));
    dir.tipo       = NO_INTERNO;
    dir.proxima    = SEM_PAGINA;
    dir.numero_chaves = total - meio - 1;
    for (int i = 0; i < dir.numero_chaves; i++) {
        dir.chaves[i] = no->chaves[meio + 1 + i];
    }
    for (int i = 0; i <= dir.numero_chaves; i++) {
        dir.filhos[i] = no->filhos[meio + 1 + i];
    }

    *chave_promovida = no->chaves[meio];
    no->numero_chaves = meio;

    if (gravar_no(pagina, no) != 0) return INSERE_ERRO;
    if (gravar_no(nova, &dir) != 0) return INSERE_ERRO;

    *nova_pagina = nova;
    return INSERE_DIVIDIU;
}

/* ---------- Insercao ---------- */

/*
 Insercao recursiva no sub-arvore enraizada na pagina 'pagina'.
 Retorna INSERE_OK, INSERE_DUPLICADO, INSERE_ERRO ou INSERE_DIVIDIU; neste
 ultimo caso *chave_promovida e *nova_pagina descrevem o que o pai deve
 inserir (chave de roteamento + ponteiro para a nova pagina a direita).*/
static int inserir_registro(int pagina, const Registro *reg, int32_t *chave_promovida, int *nova_pagina) {
    No no;
    if (ler_no(pagina, &no) != 0) return INSERE_ERRO;

    if (no.tipo == NO_FOLHA) {
        int pos = 0;
        while (pos < no.numero_chaves && no.registros[pos].id < reg->id) {
            pos++;
        }
        if (pos < no.numero_chaves && no.registros[pos].id == reg->id) {
            return INSERE_DUPLICADO;
        }

        for (int i = no.numero_chaves; i > pos; i--) {
            no.registros[i] = no.registros[i - 1];
        }
        no.registros[pos] = *reg;
        no.numero_chaves++;

        if (no.numero_chaves <= MAX_CHAVES) {
            return (gravar_no(pagina, &no) == 0) ? INSERE_OK : INSERE_ERRO;
        }
        return dividir_folha(pagina, &no, chave_promovida, nova_pagina);
    }

    /* No interno: desce para o filho adequado */
    int i = indice_filho(&no, reg->id);
    int32_t chave_filho;
    int pagina_filho;
    int r = inserir_registro(no.filhos[i], reg, &chave_filho, &pagina_filho);
    if (r != INSERE_DIVIDIU) {
        return r;
    }

    /* O filho dividiu: insere a chave de roteamento e o novo ponteiro aqui */
    for (int j = no.numero_chaves; j > i; j--) {
        no.chaves[j] = no.chaves[j - 1];
    }
    for (int j = no.numero_chaves + 1; j > i + 1; j--) {
        no.filhos[j] = no.filhos[j - 1];
    }
    no.chaves[i]     = chave_filho;
    no.filhos[i + 1] = pagina_filho;
    no.numero_chaves++;

    if (no.numero_chaves <= MAX_CHAVES) {
        return (gravar_no(pagina, &no) == 0) ? INSERE_OK : INSERE_ERRO;
    }
    return dividir_interno(pagina, &no, chave_promovida, nova_pagina);
}

/*
 Insere um registro (chave = id) na arvore.
 Se a raiz for dividida, cria uma nova raiz interna (a arvore cresce em altura).
 Retorna INSERE_OK, INSERE_DUPLICADO (chave ja existe) ou INSERE_ERRO.*/
int inserir_valor(const Registro *reg) {
    int raiz = ler_raiz();
    if (raiz < 0) return INSERE_ERRO;

    int32_t chave_promovida;
    int nova_pagina;
    int r = inserir_registro(raiz, reg, &chave_promovida, &nova_pagina);

    if (r == INSERE_DIVIDIU) {
        int nova_raiz = alocar_pagina();
        if (nova_raiz < 0) return INSERE_ERRO;

        No no;
        memset(&no, 0, sizeof(no));
        no.tipo       = NO_INTERNO;
        no.proxima    = SEM_PAGINA;
        no.numero_chaves = 1;
        no.chaves[0]  = chave_promovida;
        no.filhos[0]  = raiz;
        no.filhos[1]  = nova_pagina;

        if (gravar_no(nova_raiz, &no) != 0) return INSERE_ERRO;
        if (gravar_raiz(nova_raiz) != 0)    return INSERE_ERRO;
        return INSERE_OK;
    }
    return r;
}

/* ---------- Percurso sequencial ---------- */

/*
 percorrer_registros(cb, arg)
 Percorre TODOS os registros da arvore em ordem crescente de id: desce da raiz
 ate a folha mais a esquerda (sempre por filhos[0]) e segue a lista encadeada
 de folhas (campo 'proxima').

 Para cada registro chama cb(registro, arg). Se o callback devolver valor
 diferente de 0, o percurso para antes do fim (parada antecipada, nao e' erro).
 Cada no e' copiado por ler_no() e a pagina e' solta antes do callback, entao
 o callback pode usar a arvore/cache sem risco de deixar paginas fixadas.

 Retorna o numero de registros visitados (>= 0), ou -1 em caso de erro
 (falha de leitura ou estrutura corrompida, como ciclo na lista de folhas).*/
int percorrer_registros(int (*cb)(const Registro *, void *), void *arg) {
    if (cb == NULL) return -1;

    int pagina = ler_raiz();
    if (pagina < 0) return -1;

    int limite = total_paginas_alocadas();   /* nenhum caminho valido passa disso */
    No no;

    /* desce ate a folha mais a esquerda */
    for (int passos = 0; ; passos++) {
        if (passos > limite) return -1;
        if (ler_no(pagina, &no) != 0) return -1;
        if (no.tipo == NO_FOLHA) break;
        pagina = no.filhos[0];
    }

    /* segue a lista encadeada de folhas */
    int visitados = 0;
    for (int folhas = 0; ; folhas++) {
        if (folhas > limite) return -1;      /* protecao contra ciclo */

        for (int i = 0; i < no.numero_chaves; i++) {
            visitados++;
            if (cb(&no.registros[i], arg) != 0) {
                return visitados;            /* parada antecipada */
            }
        }

        if (no.proxima == SEM_PAGINA) break;
        pagina = no.proxima;
        if (ler_no(pagina, &no) != 0) return -1;
    }
    return visitados;
}

/* ---------- Impressao (apoio a demonstracao) ---------- */

static void imprimir_no(int pagina, int nivel) {
    No no;
    if (ler_no(pagina, &no) != 0) return;

    printf("%*s", nivel * 4, "");
    if (no.tipo == NO_FOLHA) {
        printf("[Pagina %d | FOLHA]   registros:", pagina);
        for (int i = 0; i < no.numero_chaves; i++) {
            printf(" (id=%d, valor=%d)", no.registros[i].id, no.registros[i].valor);
        }
        if (no.proxima != SEM_PAGINA) printf("  -> proxima folha: pagina %d", no.proxima);
        printf("\n");
    } else {
        printf("[Pagina %d | INTERNO] chaves:", pagina);
        for (int i = 0; i < no.numero_chaves; i++) printf(" %d", no.chaves[i]);
        printf("  | filhos (paginas):");
        for (int i = 0; i <= no.numero_chaves; i++) printf(" %d", no.filhos[i]);
        printf("\n");
        for (int i = 0; i <= no.numero_chaves; i++) {
            imprimir_no(no.filhos[i], nivel + 1);
        }
    }
}

void imprimir_arvore(void) {
    int raiz = ler_raiz();
    if (raiz >= 0) imprimir_no(raiz, 0);
}
#endif
