/* ============================================================
 *          CACHE DE PAGINAS (LRU / STEAL / NO-FORCE)
 * ============================================================
 *
 * Politicas adotadas:
 *   - STEAL:    uma pagina suja (dirty) PODE ser expulsa do cache
 *               antes do fim da transacao/programa, desde que seja
 *               gravada em disco imediatamente antes da expulsao.
 *   - NO-FORCE: uma pagina suja NAO precisa ser gravada em disco
 *               no momento em que e' solta (unpin); a gravacao so
 *               acontece quando a pagina e' expulsa (_expulsa) ou
 *               quando descarrega() e' chamada explicitamente.
 *
 * Substituicao de paginas: algoritmo LRU (Least Recently Used) -
 * dentre os frames sem fixacoes, expulsa-se sempre aquele cuja
 * pagina foi usada ha mais tempo.
 */

#ifndef NUM_FRAMES
#define NUM_FRAMES 4   // capacidade do cache: numero de frames (paginas) em memoria
#endif                 // pode ser sobrescrita em tempo de compilacao (ex.: -DNUM_FRAMES=3),
                        // o que os testes de aceitacao usam para variar a capacidade do cache

typedef struct {
    int pagina;                            // numero da pagina contida no frame (-1 = frame livre)
    unsigned char dados[TAMANHO_PAGINA];   // conteudo da pagina, em memoria
    int fixacoes;                          // contador de fixacoes (pin count); >0 impede expulsao
    int suja;                              // 1 se a pagina foi modificada e ainda nao gravada (dirty)
    long ultimo_uso;                       // "relogio" logico usado pelo algoritmo LRU
} Frame;

typedef struct {
    Frame frames[NUM_FRAMES];
    int capacidade;   // numero total de frames do cache
    int uso;          // numero de frames atualmente ocupados (paginas residentes)
    long relogio;      // contador logico, incrementado a cada acesso, usado para o LRU
    long acertos;      // numero de acertos (cache hits)
    long faltas;      // numero de faltas (cache misses)
} CachePaginas;

static CachePaginas cache = { .capacidade = NUM_FRAMES };
static int cache_inicializado = 0;

// Inicializa (uma unica vez) todos os frames do cache como vazios
static void inicializa_cache(void) {
    if (cache_inicializado) {
        return;
    }
    for (int i = 0; i < NUM_FRAMES; i++) {
        cache.frames[i].pagina = -1;
        cache.frames[i].fixacoes = 0;
        cache.frames[i].suja = 0;
        cache.frames[i].ultimo_uso = 0;
        memset(cache.frames[i].dados, 0, TAMANHO_PAGINA);
    }
    cache.uso = 0;
    cache.relogio = 0;
    cache.acertos = 0;
    cache.faltas = 0;
    cache_inicializado = 1;
}

// Procura o indice do frame que contem a pagina n. Retorna -1 se a pagina nao estiver em cache.
static int busca_frame(int n) {
    for (int i = 0; i < cache.capacidade; i++) {
        if (cache.frames[i].pagina == n) {
            return i;
        }
    }
    return -1;
}

/*
 descarrega(n)
 Grava em disco o conteudo do frame que contem a pagina n, caso essa pagina esteja
 suja (dirty). Se a pagina nao estiver em cache, ou nao estiver suja, nao faz nada.
 Apos gravar com sucesso, o frame deixa de estar sujo.
 Retorna 0 em caso de sucesso (inclusive quando nao havia nada a gravar),
 ou -1 em caso de erro de escrita.
*/
int descarrega(int n) {
    inicializa_cache();

    int idx = busca_frame(n);
    if (idx == -1) {
        return 0; // pagina nao esta em cache: nada a descarregar
    }
    if (!cache.frames[idx].suja) {
        return 0; // pagina limpa: nada a gravar
    }

    if (escreve_pagina(n, cache.frames[idx].dados, TAMANHO_PAGINA) != 0) {
        fprintf(stderr, "Erro: falha ao descarregar a pagina %d para o disco\n", n);
        return -1;
    }

    cache.frames[idx].suja = 0;
    return 0;
}

/*
 _expulsa()
 Escolhe, entre os frames sem fixacoes (fixacoes == 0), aquele cuja pagina foi usada
 ha mais tempo (menor 'ultimo_uso'), segundo o algoritmo LRU, e libera esse frame.

 Politica STEAL: se a pagina escolhida estiver suja, ela e' gravada em disco
 (via descarrega) ANTES de o frame ser liberado - nunca se expulsa uma pagina
 suja sem antes grava-la.

 Retorna o indice do frame liberado (pronto para reuso), ou -1 se nao houver
 nenhum frame elegivel para expulsao (todos os frames estao fixados).
*/
static int _expulsa(void) {
    int idx_vitima = -1;
    long menor_uso = 0;

    for (int i = 0; i < cache.capacidade; i++) {
        if (cache.frames[i].pagina == -1) {
            continue; // frame vazio: nao ha pagina para expulsar aqui
        }
        if (cache.frames[i].fixacoes > 0) {
            continue; // pagina fixada nunca pode ser expulsa
        }
        if (idx_vitima == -1 || cache.frames[i].ultimo_uso < menor_uso) {
            idx_vitima = i;
            menor_uso = cache.frames[i].ultimo_uso;
        }
    }

    if (idx_vitima == -1) {
        fprintf(stderr, "Erro: cache cheio e nao ha frames elegiveis para expulsao (todos fixados)\n");
        return -1;
    }

    // STEAL: garante a gravacao de uma pagina suja antes de expulsa-la
    if (cache.frames[idx_vitima].suja) {
        if (descarrega(cache.frames[idx_vitima].pagina) != 0) {
            fprintf(stderr, "Erro: falha ao gravar pagina suja %d antes da expulsao\n",
                    cache.frames[idx_vitima].pagina);
            return -1;
        }
    }

    cache.frames[idx_vitima].pagina = -1;
    cache.frames[idx_vitima].suja = 0;
    cache.frames[idx_vitima].fixacoes = 0;
    cache.uso--;

    return idx_vitima;
}

/*
 fixa(n)
 "Fixa" (pin) a pagina n no cache e retorna um ponteiro para o seu buffer de
 TAMANHO_PAGINA bytes, trazendo-a do disco caso ainda nao esteja em memoria.

 - ACERTO (hit): se a pagina ja estiver em algum frame, apenas incrementa o
   contador de fixacoes e devolve A MESMA REFERENCIA (o mesmo ponteiro) ja
   associada a essa pagina - nunca uma copia nova.
 - FALTA (miss): procura um frame livre; se nao houver nenhum (uso == capacidade),
   chama _expulsa() para liberar um frame pelo algoritmo LRU, respeitando as
   politicas STEAL/NO-FORCE, e entao le a pagina do disco para esse frame.

 Retorna NULL em caso de erro (numero de pagina invalido, ou cache cheio com
 todos os frames fixados, ou falha de leitura em disco).
*/
unsigned char *fixa(int n) {
    inicializa_cache();

    if (n < 0 || n >= total_paginas_alocadas()) {
        fprintf(stderr, "Erro: tentativa de fixar pagina invalida (%d)\n", n);
        return NULL;
    }

    int idx = busca_frame(n);
    if (idx != -1) {
        // ACERTO: a pagina ja esta em cache -> devolve a mesma referencia
        cache.acertos++;
        cache.frames[idx].fixacoes++;
        cache.frames[idx].ultimo_uso = ++cache.relogio;
        return cache.frames[idx].dados;
    }

    // FALTA: a pagina precisa ser trazida do disco
    cache.faltas++;

    if (cache.uso >= cache.capacidade) {
        idx = _expulsa();
        if (idx == -1) {
            return NULL; // cache cheio, todos os frames fixados
        }
    } else {
        for (int i = 0; i < cache.capacidade; i++) {
            if (cache.frames[i].pagina == -1) {
                idx = i;
                break;
            }
        }
    }

    if (ler_pagina(n, cache.frames[idx].dados, TAMANHO_PAGINA) != 0) {
        fprintf(stderr, "Erro: falha ao carregar a pagina %d no cache\n", n);
        return NULL;
    }

    cache.frames[idx].pagina = n;
    cache.frames[idx].suja = 0;
    cache.frames[idx].fixacoes = 1;
    cache.frames[idx].ultimo_uso = ++cache.relogio;
    cache.uso++;

    return cache.frames[idx].dados;
}

/*
 solta(n, suja)
 "Solta" (unpin) a pagina n, decrementando seu contador de fixacoes em 1.
 O parametro 'suja' indica se a pagina foi modificada enquanto estava fixada:
 se for diferente de 0, o frame passa a ser marcado como sujo (dirty).

 Politica NO-FORCE: soltar uma pagina suja NAO a grava em disco imediatamente;
 a gravacao so ocorre quando o frame e' expulso (_expulsa) ou quando
 descarrega() e' chamada explicitamente.

 Retorna 0 em caso de sucesso, ou -1 em caso de erro (pagina nao esta em cache,
 ou ja estava com o contador de fixacoes em zero).
*/
int solta(int n, int suja) {
    inicializa_cache();

    int idx = busca_frame(n);
    if (idx == -1) {
        fprintf(stderr, "Erro: tentativa de soltar a pagina %d, que nao esta em cache\n", n);
        return -1;
    }
    if (cache.frames[idx].fixacoes <= 0) {
        fprintf(stderr, "Erro: pagina %d ja estava solta (fixacoes == 0)\n", n);
        return -1;
    }

    cache.frames[idx].fixacoes--;
    if (suja) {
        cache.frames[idx].suja = 1; // NO-FORCE: apenas marca; nao grava agora
    }
    return 0;
}

/*
 Imprime as estatisticas atuais do cache: capacidade, uso, acertos e faltas.
*/
void imprime_estatisticas_cache(void) {
    printf("[cache] capacidade=%d uso=%d acertos=%ld faltas=%ld\n",
           cache.capacidade, cache.uso, cache.acertos, cache.faltas);
}