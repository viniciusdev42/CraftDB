#include <stdio.h>
#include <stdlib.h>
#include<stdint.h>
#include <string.h>

#include "Pager.c"

#ifndef NUM_FRAMES
#define NUM_FRAMES 4
#endif
                
typedef struct {
    int pagina;                            
    unsigned char dados[TAMANHO_PAGINA];  
    int fixacoes;                          
    int suja;
    long ultimo_uso;
} Frame;

typedef struct {
    Frame frames[NUM_FRAMES];
    int capacidade;  
    int uso;          
    long relogio;      
    long acertos;      
    long faltas;    
} CachePaginas;

static CachePaginas cache = { .capacidade = NUM_FRAMES };
static int cache_inicializado = 0;

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

static int busca_frame(int n) {
    for (int i = 0; i < cache.capacidade; i++) {
        if (cache.frames[i].pagina == n) {
            return i;
        }
    }
    return -1;
}

int descarrega(int n) {
    inicializa_cache();

    int idx = busca_frame(n);
    if (idx == -1) {
        return 0; 
    }
    if (!cache.frames[idx].suja) {
        return 0; 
    }

    if (escreve_pagina(n, cache.frames[idx].dados, TAMANHO_PAGINA) != 0) {
        fprintf(stderr, "Erro: falha ao descarregar a pagina %d para o disco\n", n);
        return -1;
    }

    cache.frames[idx].suja = 0;
    return 0;
}

static int _expulsa(void) {
    int idx_vitima = -1;
    long menor_uso = 0;

    for (int i = 0; i < cache.capacidade; i++) {
        if (cache.frames[i].pagina == -1) {
            continue; 
        }
        if (cache.frames[i].fixacoes > 0) {
            continue;
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

unsigned char *fixa(int n) {
    inicializa_cache();

    if (n < 0 || n >= total_paginas_alocadas()) {
        fprintf(stderr, "Erro: tentativa de fixar pagina invalida (%d)\n", n);
        return NULL;
    }

    int idx = busca_frame(n);
    if (idx != -1) {
        cache.acertos++;
        cache.frames[idx].fixacoes++;
        cache.frames[idx].ultimo_uso = ++cache.relogio;
        return cache.frames[idx].dados;
    }
    
    cache.faltas++;

    if (cache.uso >= cache.capacidade) {
        idx = _expulsa();
        if (idx == -1) {
            return NULL; 
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
        cache.frames[idx].suja = 1; 
    }
    return 0;
}

void imprime_estatisticas_cache(void) {
    printf("[cache] capacidade=%d uso=%d acertos=%ld faltas=%ld\n",
           cache.capacidade, cache.uso, cache.acertos, cache.faltas);
}
