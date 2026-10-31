#include <stdio.h>
#include <stdlib.h>
#include<stdint.h>
#include <string.h>
#include <unistd.h> // fsync - Gravação em Disco 
#include<fcntl.h> // struct flock - Isolamento de Páginas
#include <ctype.h> // Tokenizador
#include <errno.h>
#include <limits.h>
#include <stdarg.h>

#define TAMANHO_PAGINA 4096   // 4KB
#define TAMANHO_CABECALHO 16  // 16B   
#define TAMANHO_REGISTRO 8    // 8B 
#define REGISTROS_POR_PAGINA ((TAMANHO_PAGINA - TAMANHO_CABECALHO) / TAMANHO_REGISTRO) // 510B

#define ARQUIVO_BD "craft-db.dat"

/* Contador global de leituras FISICAS de pagina em disco (incrementado apenas
   dentro de ler_pagina(), em toda leitura bem-sucedida). Serve como uma sonda
   independente das estatisticas do cache, util para testes de aceitacao que
   precisam confirmar que uma pagina foi de fato lida do disco (ou nao). */
static long contador_leituras_disco = 0;

/* Struct utilizada para operações de serialização e desserialização */

typedef struct {
    int32_t id;
    int32_t valor;
} Registro;

// Retorna o tamanho atual do arquivo do banco, em bytes
static long tamanho_arquivo(void) {
    long tamanho = 0;
    FILE *f = fopen(ARQUIVO_BD, "rb");
    if (f != NULL) {
        fseek(f, 0, SEEK_END);
        tamanho = ftell(f);
        fclose(f);
    }
    return tamanho;
}

// Retorna quantas paginas ja foram alocadas (existem) no arquivo 
int total_paginas_alocadas(void) {
    return (int)(tamanho_arquivo() / TAMANHO_PAGINA);
}

/*
 Trava (ou destrava) o intervalo de bytes pertencente a pagina n.
 Como a trava cobre apenas os bytes da propria pagina, uma escrita na pagina n 
 nunca disputa nem interfere com os bytes de qualquer outra pagina m != n,
 mesmo havendo acessos concorrentes ao arquivo.
 O tipo_trava deve ser F_RDLCK (leitura), F_WRLCK (escrita) ou F_UNLCK.
*/
static int trava_pagina(FILE *f, int n, short tipo_trava) {
    struct flock trava;
    memset(&trava, 0, sizeof(trava));
    trava.l_type   = tipo_trava;
    trava.l_whence = SEEK_SET;
    trava.l_start  = (long)n * TAMANHO_PAGINA;
    trava.l_len    = TAMANHO_PAGINA;

    if (fcntl(fileno(f), F_SETLKW, &trava) == -1) {
        fprintf(stderr, "Erro: falha ao travar a pagina %d\n", n);
        return -1;
    }
    return 0;
}

/* 
  Escreve 4KB de buffer na posicao da pagina n do arquivo.
  A escrita fica delimitada ao intervalo exato de bytes da pagina n
  possuindo o mesmo offset de inicio, e o mesmo tamanho fixo de TAMANHO_PAGINA.
  É protegida por uma trava (F_WRLCK) restrita a esse mesmo intervalo,
  garantindo que os bytes de outras paginas nunca sejam tocados.
  É usada por escreve_pagina() e aloca_pagina()
*/
static int escreve_pagina_buffer(int n, const unsigned char *buffer) {
   FILE *f = fopen(ARQUIVO_BD, "r+b");
    if (f == NULL) {
        f = fopen(ARQUIVO_BD, "w+b");
        if (f == NULL) {
            fprintf(stderr, "Erro: nao foi possivel abrir/criar '%s'\n", ARQUIVO_BD);
            return -1;
        }
    }

    if (trava_pagina(f, n, F_WRLCK) != 0) {
        fclose(f);
        return -1;
    }

    fseek(f, (long)n * TAMANHO_PAGINA, SEEK_SET);
    if (fwrite(buffer, 1, TAMANHO_PAGINA, f) != TAMANHO_PAGINA) {
        fprintf(stderr, "Erro: falha ao escrever a pagina %d\n", n);
        trava_pagina(f, n, F_UNLCK);
        fclose(f);
        return -1;
    }

    fflush(f);
    fsync(fileno(f));

    trava_pagina(f, n, F_UNLCK);
    fclose(f);
    return 0;
}

/*
Aloca uma nova pagina no fim do arquivo e retorna o numero da pagina recem criada (ou -1 em caso de erro) 
*/
int aloca_pagina(void) {
    int nova_pagina = total_paginas_alocadas();
    unsigned char pagina_vazia[TAMANHO_PAGINA];
    memset(pagina_vazia, 0, TAMANHO_PAGINA);

    if (escreve_pagina_buffer(nova_pagina, pagina_vazia) != 0) {
        return -1;
    }
    return nova_pagina;
}

/*
 Garante que os dados mantenham-se persistidos.
 Retorna 0 em caso de sucesso,ou -1 em caso de erro
*/

int sincroniza_dados(void) {
    FILE *f = fopen(ARQUIVO_BD, "r+b");
    if (f == NULL) {
        fprintf(stderr, "Erro: nao foi possivel abrir '%s' para sincronizar\n", ARQUIVO_BD);
        return -1;
    }
    fflush(f);
    if (fsync(fileno(f)) != 0) {
        fprintf(stderr, "Erro: falha ao sincronizar dados com o disco\n");
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

/* Escreve um uint32_t em 4 bytes, sempre em little-endian, byte a byte.
 * Nao depende do endianness nativo da CPU (nao usa memcpy nem union). */
static void serializa_uint32_le(uint32_t valor, unsigned char *destino) {
    destino[0] = (unsigned char)(valor & 0xFF);
    destino[1] = (unsigned char)((valor >> 8) & 0xFF);
    destino[2] = (unsigned char)((valor >> 16) & 0xFF);
    destino[3] = (unsigned char)((valor >> 24) & 0xFF);
}

/* Le 4 bytes em little-endian e reconstroi o uint32_t original. */
static uint32_t desserializa_uint32_le(const unsigned char *origem) {
    return   (uint32_t)origem[0]
           | ((uint32_t)origem[1] << 8)
           | ((uint32_t)origem[2] << 16)
           | ((uint32_t)origem[3] << 24);
}

void serializa_registro(const Registro *registro, unsigned char *buffer) {
    serializa_uint32_le((uint32_t)registro->id, buffer);
    serializa_uint32_le((uint32_t)registro->valor, buffer + 4);
}

void desserializa_registro(const unsigned char *buffer, Registro *registro) {
    registro->id    = (int32_t)desserializa_uint32_le(buffer);
    registro->valor = (int32_t)desserializa_uint32_le(buffer + 4);
}

/*
 Lê 4KB da página n do arquivo e para dentro do buffer.
 Valida o numero da pagina (caso exista) e o tamanho do buffer recebido (deve ser exatamente 4KB).
 Retorna 0 em caso de sucesso,ou -1 em caso de erro.
 */

int ler_pagina(int n, unsigned char *buffer, size_t tamanho_buffer) {
     if (buffer == NULL || tamanho_buffer != TAMANHO_PAGINA) {
        fprintf(stderr, "Erro: buffer invalido (deve ter exatamente %d bytes)\n", TAMANHO_PAGINA);
        return -1;
    }
    if (n < 0 || n >= total_paginas_alocadas()) {
        fprintf(stderr, "Erro: numero de pagina invalido (%d). Paginas alocadas: %d\n",
                n, total_paginas_alocadas());
        return -1;
    }

    FILE *f = fopen(ARQUIVO_BD, "rb");
    if (f == NULL) {
        fprintf(stderr, "Erro: nao foi possivel abrir '%s'\n", ARQUIVO_BD);
        return -1;
    }

    if (trava_pagina(f, n, F_RDLCK) != 0) {
        fclose(f);
        return -1;
    }

    fseek(f, (long)n * TAMANHO_PAGINA, SEEK_SET);
    if (fread(buffer, 1, TAMANHO_PAGINA, f) != TAMANHO_PAGINA) {
        fprintf(stderr, "Erro: falha ao ler a pagina %d\n", n);
        trava_pagina(f, n, F_UNLCK);
        fclose(f);
        return -1;
    }

    trava_pagina(f, n, F_UNLCK);
    fclose(f);
    contador_leituras_disco++;
    return 0;    
 }

/*
  Escreve os 4KB de buffer na página n do arquivo,a qual precisa ter sido alocada previamente.
  Tambem valida o numero da pagina e o tamanho do buffer recebido.
  Retorna 0 em caso de sucesso,ou -1 em caso de erro.
 */

int escreve_pagina(int n, unsigned char *buffer, size_t tamanho_buffer) {
    if (buffer == NULL || tamanho_buffer != TAMANHO_PAGINA) {
        fprintf(stderr, "Erro: buffer invalido (deve ter exatamente %d bytes)\n", TAMANHO_PAGINA);
        return -1;
    }
    if (n < 0 || n >= total_paginas_alocadas()) {
        fprintf(stderr, "Erro: pagina %d nao foi alocada. Use aloca_pagina() antes de escrever.\n", n);
        return -1;
    }

    return escreve_pagina_buffer(n, buffer);
}

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
 *     [ 4.. 7] num_chaves  (quantidade de chaves/registros em uso)
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
 *                            por serializa_registro). A chave e' o 'id'.
 *
 * Cada no possui no maximo MAX_CHAVES (3) chaves. Quando uma insercao
 * faria um no ter 4 chaves, o no e' DIVIDIDO (split).
 * Fusao e redistribuicao NAO sao implementadas (nao ha remocao).
 */

#define MAX_CHAVES   3
#define NO_INTERNO   0
#define NO_FOLHA     1
#define SEM_PAGINA   (-1)
#define PAGINA_META  0

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
    int32_t  num_chaves;
    int32_t  proxima;                       /* so para folhas */
    int32_t  chaves[MAX_CHAVES + 1];        /* so para nos internos */
    int32_t  filhos[MAX_CHAVES + 2];        /* so para nos internos */
    Registro registros[MAX_CHAVES + 1];     /* so para folhas */
} No;

/* ---------- Serializacao de nos (No <-> bytes da pagina) ---------- */

static void serializa_no(const No *no, unsigned char *pagina) {
    memset(pagina, 0, TAMANHO_PAGINA);
    serializa_uint32_le((uint32_t)no->tipo,       pagina);
    serializa_uint32_le((uint32_t)no->num_chaves, pagina + 4);
    serializa_uint32_le((uint32_t)no->proxima,    pagina + 8);

    if (no->tipo == NO_FOLHA) {
        for (int i = 0; i < no->num_chaves; i++) {
            serializa_registro(&no->registros[i],
                               pagina + OFFSET_REGISTROS + i * TAMANHO_REGISTRO);
        }
    } else {
        for (int i = 0; i < no->num_chaves; i++) {
            serializa_uint32_le((uint32_t)no->chaves[i], pagina + OFFSET_CHAVES + i * 4);
        }
        for (int i = 0; i <= no->num_chaves; i++) {
            serializa_uint32_le((uint32_t)no->filhos[i], pagina + OFFSET_FILHOS + i * 4);
        }
    }
}

static void desserializa_no(const unsigned char *pagina, No *no) {
    memset(no, 0, sizeof(*no));
    no->tipo       = (int32_t)desserializa_uint32_le(pagina);
    no->num_chaves = (int32_t)desserializa_uint32_le(pagina + 4);
    no->proxima    = (int32_t)desserializa_uint32_le(pagina + 8);

    if (no->num_chaves < 0 || no->num_chaves > MAX_CHAVES) {
        no->num_chaves = 0; /* pagina corrompida/vazia: trata como no sem chaves */
    }

    if (no->tipo == NO_FOLHA) {
        for (int i = 0; i < no->num_chaves; i++) {
            desserializa_registro(pagina + OFFSET_REGISTROS + i * TAMANHO_REGISTRO,
                                  &no->registros[i]);
        }
    } else {
        for (int i = 0; i < no->num_chaves; i++) {
            no->chaves[i] = (int32_t)desserializa_uint32_le(pagina + OFFSET_CHAVES + i * 4);
        }
        for (int i = 0; i <= no->num_chaves; i++) {
            no->filhos[i] = (int32_t)desserializa_uint32_le(pagina + OFFSET_FILHOS + i * 4);
        }
    }
}

/* ---------- Acesso a nos via cache de paginas (fixa/solta) ---------- */

/* Le o no da pagina n: fixa a pagina, copia para 'no' e solta (sem sujar). */
static int le_no(int n, No *no) {
    unsigned char *pagina = fixa(n);
    if (pagina == NULL) return -1;
    desserializa_no(pagina, no);
    solta(n, 0);
    return 0;
}

/* Grava o no na pagina n: fixa a pagina, serializa e solta marcando-a suja
   (NO-FORCE: o disco so e' atualizado na expulsao ou em descarrega()). */
static int grava_no(int n, const No *no) {
    unsigned char *pagina = fixa(n);
    if (pagina == NULL) return -1;
    serializa_no(no, pagina);
    solta(n, 1);
    return 0;
}

/* ---------- Pagina de metadados (guarda a raiz) ---------- */

static int le_raiz(void) {
    unsigned char *pagina = fixa(PAGINA_META);
    if (pagina == NULL) return -1;
    int raiz = (int)(int32_t)desserializa_uint32_le(pagina);
    solta(PAGINA_META, 0);
    return raiz;
}

static int grava_raiz(int raiz) {
    unsigned char *pagina = fixa(PAGINA_META);
    if (pagina == NULL) return -1;
    serializa_uint32_le((uint32_t)raiz, pagina);
    solta(PAGINA_META, 1);
    return 0;
}

/* Cria a arvore (pagina de metadados + raiz folha vazia) se o arquivo ainda
   estiver vazio. Retorna 0 em caso de sucesso, -1 em caso de erro. */
int inicializa_arvore(void) {
    if (total_paginas_alocadas() == 0) {
        if (aloca_pagina() != PAGINA_META) return -1;

        int pagina_raiz = aloca_pagina();
        if (pagina_raiz < 0) return -1;

        No folha;
        memset(&folha, 0, sizeof(folha));
        folha.tipo    = NO_FOLHA;
        folha.proxima = SEM_PAGINA;

        if (grava_no(pagina_raiz, &folha) != 0) return -1;
        if (grava_raiz(pagina_raiz) != 0) return -1;
    }
    return (le_raiz() < 0) ? -1 : 0;
}

/* ---------- Busca ---------- */

/* Dado um no interno, devolve o indice do filho por onde a chave deve descer. */
static int indice_filho(const No *no, int32_t chave) {
    int i = 0;
    while (i < no->num_chaves && chave >= no->chaves[i]) {
        i++;
    }
    return i;
}

/*
 Busca a chave na arvore, descendo da raiz ate a folha.
 Retorna 0 se encontrou (copiando o registro para *saida, se nao for NULL),
 1 se a chave nao existe, ou -1 em caso de erro.
*/
int buscar_valor(int32_t chave, Registro *saida) {
    int pagina = le_raiz();
    if (pagina < 0) return -1;

    No no;
    for (;;) {
        if (le_no(pagina, &no) != 0) return -1;
        if (no.tipo == NO_FOLHA) break;
        pagina = no.filhos[indice_filho(&no, chave)];
    }

    for (int i = 0; i < no.num_chaves; i++) {
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
 Retorna INSERE_DIVIDIU em caso de sucesso, ou INSERE_ERRO.
*/
int dividir_folha(int pagina, No *no, int32_t *chave_promovida, int *nova_pagina) {
    int nova = aloca_pagina();
    if (nova < 0) return INSERE_ERRO;

    int total = no->num_chaves;          /* MAX_CHAVES + 1 */
    int meio  = (total + 1) / 2;         /* 2 registros ficam na esquerda */

    No dir;
    memset(&dir, 0, sizeof(dir));
    dir.tipo       = NO_FOLHA;
    dir.num_chaves = total - meio;
    for (int i = 0; i < dir.num_chaves; i++) {
        dir.registros[i] = no->registros[meio + i];
    }

    dir.proxima = no->proxima;           /* mantem a lista encadeada de folhas */
    no->proxima = nova;
    no->num_chaves = meio;

    if (grava_no(pagina, no) != 0) return INSERE_ERRO;
    if (grava_no(nova, &dir) != 0) return INSERE_ERRO;

    *chave_promovida = dir.registros[0].id;
    *nova_pagina = nova;
    return INSERE_DIVIDIU;
}

/*
 Divide um NO INTERNO que ficou com MAX_CHAVES+1 (4) chaves e 5 filhos.
 - O no original fica com as 2 primeiras chaves e 3 filhos.
 - A 3a chave SOBE para o pai (nao permanece em nenhum dos dois nos).
 - Uma nova pagina recebe a 4a chave e os 2 ultimos filhos.
 Retorna INSERE_DIVIDIU em caso de sucesso, ou INSERE_ERRO.
*/
int dividir_interno(int pagina, No *no, int32_t *chave_promovida, int *nova_pagina) {
    int nova = aloca_pagina();
    if (nova < 0) return INSERE_ERRO;

    int total = no->num_chaves;          /* MAX_CHAVES + 1 */
    int meio  = total / 2;               /* indice da chave que sobe */

    No dir;
    memset(&dir, 0, sizeof(dir));
    dir.tipo       = NO_INTERNO;
    dir.proxima    = SEM_PAGINA;
    dir.num_chaves = total - meio - 1;
    for (int i = 0; i < dir.num_chaves; i++) {
        dir.chaves[i] = no->chaves[meio + 1 + i];
    }
    for (int i = 0; i <= dir.num_chaves; i++) {
        dir.filhos[i] = no->filhos[meio + 1 + i];
    }

    *chave_promovida = no->chaves[meio];
    no->num_chaves = meio;

    if (grava_no(pagina, no) != 0) return INSERE_ERRO;
    if (grava_no(nova, &dir) != 0) return INSERE_ERRO;

    *nova_pagina = nova;
    return INSERE_DIVIDIU;
}

/* ---------- Insercao ---------- */

/*
 Insercao recursiva no sub-arvore enraizada na pagina 'pagina'.
 Retorna INSERE_OK, INSERE_DUPLICADO, INSERE_ERRO ou INSERE_DIVIDIU; neste
 ultimo caso *chave_promovida e *nova_pagina descrevem o que o pai deve
 inserir (chave de roteamento + ponteiro para a nova pagina a direita).
*/
static int inserir_registro(int pagina, const Registro *reg, int32_t *chave_promovida, int *nova_pagina) {
    No no;
    if (le_no(pagina, &no) != 0) return INSERE_ERRO;

    if (no.tipo == NO_FOLHA) {
        int pos = 0;
        while (pos < no.num_chaves && no.registros[pos].id < reg->id) {
            pos++;
        }
        if (pos < no.num_chaves && no.registros[pos].id == reg->id) {
            return INSERE_DUPLICADO;
        }

        for (int i = no.num_chaves; i > pos; i--) {
            no.registros[i] = no.registros[i - 1];
        }
        no.registros[pos] = *reg;
        no.num_chaves++;

        if (no.num_chaves <= MAX_CHAVES) {
            return (grava_no(pagina, &no) == 0) ? INSERE_OK : INSERE_ERRO;
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
    for (int j = no.num_chaves; j > i; j--) {
        no.chaves[j] = no.chaves[j - 1];
    }
    for (int j = no.num_chaves + 1; j > i + 1; j--) {
        no.filhos[j] = no.filhos[j - 1];
    }
    no.chaves[i]     = chave_filho;
    no.filhos[i + 1] = pagina_filho;
    no.num_chaves++;

    if (no.num_chaves <= MAX_CHAVES) {
        return (grava_no(pagina, &no) == 0) ? INSERE_OK : INSERE_ERRO;
    }
    return dividir_interno(pagina, &no, chave_promovida, nova_pagina);
}

/*
 Insere um registro (chave = id) na arvore.
 Se a raiz for dividida, cria uma nova raiz interna (a arvore cresce em altura).
 Retorna INSERE_OK, INSERE_DUPLICADO (chave ja existe) ou INSERE_ERRO.
*/
int inserir_valor(const Registro *reg) {
    int raiz = le_raiz();
    if (raiz < 0) return INSERE_ERRO;

    int32_t chave_promovida;
    int nova_pagina;
    int r = inserir_registro(raiz, reg, &chave_promovida, &nova_pagina);

    if (r == INSERE_DIVIDIU) {
        int nova_raiz = aloca_pagina();
        if (nova_raiz < 0) return INSERE_ERRO;

        No no;
        memset(&no, 0, sizeof(no));
        no.tipo       = NO_INTERNO;
        no.proxima    = SEM_PAGINA;
        no.num_chaves = 1;
        no.chaves[0]  = chave_promovida;
        no.filhos[0]  = raiz;
        no.filhos[1]  = nova_pagina;

        if (grava_no(nova_raiz, &no) != 0) return INSERE_ERRO;
        if (grava_raiz(nova_raiz) != 0)    return INSERE_ERRO;
        return INSERE_OK;
    }
    return r;
}

/* ---------- Impressao (apoio a demonstracao) ---------- */

static void imprimir_no(int pagina, int nivel) {
    No no;
    if (le_no(pagina, &no) != 0) return;

    printf("%*s", nivel * 4, "");
    if (no.tipo == NO_FOLHA) {
        printf("[Pagina %d | FOLHA]   registros:", pagina);
        for (int i = 0; i < no.num_chaves; i++) {
            printf(" (id=%d, valor=%d)", no.registros[i].id, no.registros[i].valor);
        }
        if (no.proxima != SEM_PAGINA) printf("  -> proxima folha: pagina %d", no.proxima);
        printf("\n");
    } else {
        printf("[Pagina %d | INTERNO] chaves:", pagina);
        for (int i = 0; i < no.num_chaves; i++) printf(" %d", no.chaves[i]);
        printf("  | filhos (paginas):");
        for (int i = 0; i <= no.num_chaves; i++) printf(" %d", no.filhos[i]);
        printf("\n");
        for (int i = 0; i <= no.num_chaves; i++) {
            imprimir_no(no.filhos[i], nivel + 1);
        }
    }
}

void imprimir_arvore(void) {
    int raiz = le_raiz();
    if (raiz >= 0) imprimir_no(raiz, 0);
}


/*
 * Tokenizer (Analisador Lexico).
 *
 * Comandos reconhecidos:
 *   CREATE TABLE nome (coluna INT, coluna CHAR, ...);
 *   INSERT INTO nome VALUES (10, 'abc');
 *   SELECT * FROM nome WHERE coluna = 10 AND outra <> 'x';
 *
 * Tipos de dados: INT e CHAR (registros de tamanho fixo de 8 bytes).
 * NAO ha: juncao, ORDER BY, agregacao, subconsulta, OR.
 * Palavras dessas construcoes devolvem TOKEN_NAO_SUPORTADO.
 */

#define TAMANHO_LEXEMA   64  

/* ------------------------------------------------------------------ */
/* Tipos de token                                                      */
/* ------------------------------------------------------------------ */

typedef enum {
    /* Palavras reservadas (comandos) */
    TOKEN_CREATE,
    TOKEN_TABLE,
    TOKEN_INSERT,
    TOKEN_INTO,
    TOKEN_VALUES,
    TOKEN_SELECT,
    TOKEN_FROM,
    TOKEN_WHERE,
    TOKEN_AND,

    /* Tipos de dados */
    TOKEN_INT,
    TOKEN_CHAR,

    /* Literais e identificadores */
    TOKEN_IDENTACAO,
    TOKEN_NUMERO,
    TOKEN_STRING,

    /* Separadores e símbolos */
    TOKEN_ESPACO,
    TOKEN_ABRE_PARENTESE,
    TOKEN_FECHA_PARENTESE,
    TOKEN_VIRGULA,
    TOKEN_PONTO_VIRGULA,
    TOKEN_ASTERISCO,

    /* Operadores de comparação (usados no WHERE) */
    TOKEN_IGUAL,
    TOKEN_DIFERENTE,
    TOKEN_MENOR,
    TOKEN_MENOR_IGUAL,
    TOKEN_MAIOR,
    TOKEN_MAIOR_IGUAL,

    /* Controle */
    TOKEN_NAO_SUPORTADO,   /* JOIN, ORDER, BY, GROUP, COUNT, OR, ...      */
    TOKEN_FIM,             /* final de texto                              */
    TOKEN_ERRO             /* caractere/lexema invalido                   */
} TipoToken;

static const char *nome_token(TipoToken token)
{
    switch (token) {
        case TOKEN_CREATE:          return "CREATE";
        case TOKEN_TABLE:           return "TABLE";
        case TOKEN_INSERT:          return "INSERT";
        case TOKEN_INTO:            return "INTO";
        case TOKEN_VALUES:          return "VALUES";
        case TOKEN_SELECT:          return "SELECT";
        case TOKEN_FROM:            return "FROM";
        case TOKEN_WHERE:           return "WHERE";
        case TOKEN_AND:             return "AND";
        case TOKEN_INT:             return "INT";
        case TOKEN_CHAR:            return "CHAR";
        case TOKEN_IDENTACAO:       return "IDENT";
        case TOKEN_NUMERO:          return "NUMERO";
        case TOKEN_STRING:          return "STRING";
        case TOKEN_ESPACO:          return "ESPACO";
        case TOKEN_ABRE_PARENTESE:  return "ABRE_PAR";
        case TOKEN_FECHA_PARENTESE: return "FECHA_PAR";
        case TOKEN_VIRGULA:         return "VIRGULA";
        case TOKEN_PONTO_VIRGULA:   return "PONTO_VIRGULA";
        case TOKEN_ASTERISCO:       return "ASTERISCO";
        case TOKEN_IGUAL:           return "IGUAL";
        case TOKEN_DIFERENTE:       return "DIFERENTE";
        case TOKEN_MENOR:           return "MENOR";
        case TOKEN_MENOR_IGUAL:     return "MENOR_IGUAL";
        case TOKEN_MAIOR:           return "MAIOR";
        case TOKEN_MAIOR_IGUAL:     return "MAIOR_IGUAL";
        case TOKEN_NAO_SUPORTADO:   return "NAO_SUPORTADO";
        case TOKEN_FIM:             return "FIM";
        case TOKEN_ERRO:            return "ERRO";
    }
    return "?";
}

/* ------------------------------------------------------------------ */
/* Estruturas                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    TipoToken tipo;
    char      lexema[TAMANHO_LEXEMA];
    int       linha;
    int       coluna;
} Token;

typedef struct {
    const char *src;     /* texto de entrada                            */
    size_t      posicao;     /* posicao atual                               */
    int         linha;
    int         coluna;
} Tokenizador;

/* ------------------------------------------------------------------ */
/* Tabela de palavras reservadas                                       */
/* A busca e sequencial e tem prioridade sobre TOKEN_IDENTACAO: se a palavra */
/* estiver aqui, nunca vira identificador.                             */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *palavra;
    TipoToken   tipo;
} Reservada;

static const Reservada RESERVADAS[] = {
    /* suportadas */
    { "CREATE", TOKEN_CREATE }, { "TABLE",  TOKEN_TABLE  },
    { "INSERT", TOKEN_INSERT }, { "INTO",   TOKEN_INTO   },
    { "VALUES", TOKEN_VALUES }, { "SELECT", TOKEN_SELECT },
    { "FROM",   TOKEN_FROM   }, { "WHERE",  TOKEN_WHERE  },
    { "AND",    TOKEN_AND    },
    { "INT",    TOKEN_INT    }, { "CHAR",   TOKEN_CHAR   },

    /* fora do escopo: OR, juncao, ORDER BY, agregacao, GROUP BY */
    { "OR",     TOKEN_NAO_SUPORTADO },
    { "JOIN",   TOKEN_NAO_SUPORTADO }, { "INNER",  TOKEN_NAO_SUPORTADO },
    { "LEFT",   TOKEN_NAO_SUPORTADO }, { "RIGHT",  TOKEN_NAO_SUPORTADO },
    { "ON",     TOKEN_NAO_SUPORTADO },
    { "ORDER",  TOKEN_NAO_SUPORTADO }, { "BY",     TOKEN_NAO_SUPORTADO },
    { "GROUP",  TOKEN_NAO_SUPORTADO }, { "HAVING", TOKEN_NAO_SUPORTADO },
    { "COUNT",  TOKEN_NAO_SUPORTADO }, { "SUM",    TOKEN_NAO_SUPORTADO },
    { "AVG",    TOKEN_NAO_SUPORTADO }, { "MIN",    TOKEN_NAO_SUPORTADO },
    { "MAX",    TOKEN_NAO_SUPORTADO },
};

#define NUM_RESERVADAS (sizeof(RESERVADAS) / sizeof(RESERVADAS[0]))

/* Comparacao sem diferenciar maiusculas/minusculas */
static int contexto_igual(const char *a, const char *b)
{
    while (*a && *b) {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static TipoToken classificar_palavra(const char *lexer)
{
    for (size_t i = 0; i < NUM_RESERVADAS; i++)
        if (contexto_igual(lexer, RESERVADAS[i].palavra))
            return RESERVADAS[i].tipo;
    return TOKEN_IDENTACAO;
}

/* ------------------------------------------------------------------ */
/* Funcoes auxiliares                                                  */
/* ------------------------------------------------------------------ */

void iniciar_tokenizador(Tokenizador *token, const char *texto)
{
    token->src    = texto;
    token->posicao    = 0;
    token->linha  = 1;
    token->coluna = 1;
}

static int olhar(const Tokenizador *t)
{
    return (unsigned char)t->src[t->posicao];
}

static int avancar(Tokenizador *t)
{
    int caractere = olhar(t);
    if (caractere == '\0')
        return '\0';
    t->posicao++;
    if (caractere == '\n') { t->linha++; t->coluna = 1; }
    else {t->coluna++; }
    return caractere;
}

static void adicionar_caractere(Token *token, size_t *n, int caractere)
{
    if (*n <TAMANHO_LEXEMA - 1)
        token->lexema[(*n)++] = (char)caractere;
    token->lexema[*n] = '\0';
}

/* ------------------------------------------------------------------ */
/* Funcao principal: devolve o proximo token, na ordem do texto        */
/* ------------------------------------------------------------------ */

Token proximo_token(Tokenizador *t)
{
    Token token;
    size_t n = 0;
    int caractere;

    memset(&token, 0, sizeof token);
    token.linha  = t->linha;
    token.coluna = t->coluna;
    caractere = olhar(t);

    /* 1. Final de texto */
    if (caractere == '\0') {
        token.tipo = TOKEN_FIM;
        return token;
    }

    /* 2. Espaco (espaco, tab, \n, \r) - uma sequencia vira um token */
    if (isspace(caractere)) {
        while (isspace(olhar(t)))
            avancar(t);
        token.tipo = TOKEN_ESPACO;
        strcpy(token.lexema, " ");
        return token;
    }

    /* 3. Simbolos de um caractere */
    switch (caractere) {
        case '(': token.tipo = TOKEN_ABRE_PARENTESE;   break;
        case ')': token.tipo = TOKEN_FECHA_PARENTESE;  break;
        case ',': token.tipo = TOKEN_VIRGULA;          break;
        case ';': token.tipo = TOKEN_PONTO_VIRGULA;    break;
        case '*': token.tipo = TOKEN_ASTERISCO;        break;
        case '=': token.tipo = TOKEN_IGUAL;            break;
        default:  break;
    }
    if (caractere == '(' || caractere == ')' || caractere == ',' || caractere == ';' ||
        caractere == '*' || caractere == '=') {
        adicionar_caractere(&token, &n, avancar(t));
        return token;
    }

    /* 4. Operadores de 1 ou 2 caracteres: < <= <> > >= != */
    if (caractere == '<' || caractere == '>' || caractere == '!') {
        adicionar_caractere(&token, &n, avancar(t));
        int proximo = olhar(t);

        if (caractere == '<') {
            if (proximo == '=')      { adicionar_caractere(&token, &n, avancar(t)); token.tipo = TOKEN_MENOR_IGUAL; }
            else if (proximo == '>') { adicionar_caractere(&token, &n, avancar(t)); token.tipo = TOKEN_DIFERENTE;   }
            else                  { token.tipo = TOKEN_MENOR; }
        } else if (caractere == '>') {
            if (proximo == '=')      { adicionar_caractere(&token, &n, avancar(t)); token.tipo = TOKEN_MAIOR_IGUAL; }
            else                  { token.tipo = TOKEN_MAIOR; }
        } else { /* '!' so e valido como "!=" */
            if (proximo == '=')      { adicionar_caractere(&token, &n, avancar(t)); token.tipo = TOKEN_DIFERENTE; }
            else                  { token.tipo = TOKEN_ERRO; }
        }
        return token;
    }

    /* 5. Literal CHAR: 'texto' (no maximo TAMANHO_REGISTRO caracteres) */
    if (caractere == '\'') {
        size_t tamanho = 0;
        avancar(t);                          /* abre aspas */
        while (olhar(t) != '\'' && olhar(t) != '\0') {
            adicionar_caractere(&token, &n, avancar(t));
            tamanho++;
        }
        if (olhar(t) != '\'') {              /* chegou ao fim sem fechar */
            token.tipo = TOKEN_ERRO;
            return token;
        }
        avancar(t);                          /* fecha aspas */
        token.tipo = (tamanho > TAMANHO_REGISTRO) ? TOKEN_ERRO : TOKEN_STRING;
        return token;
    }

    /* 6. Literal INT: sequencia de digitos */
    if (isdigit(caractere)) {
        while (isdigit(olhar(t)))
            adicionar_caractere(&token, &n, avancar(t));
        /* numero colado em letra (ex.: 12abc) e invalido */
        if (isalpha(olhar(t)) || olhar(t) == '_') {
            while (isalnum(olhar(t)) || olhar(t) == '_')
                adicionar_caractere(&token, &n, avancar(t));
            token.tipo = TOKEN_ERRO;
        } else {
            token.tipo = TOKEN_NUMERO;
        }
        return token;
    }

    /* 7. Identificador ou palavra reservada */
    if (isalpha(caractere) || caractere == '_') {
        int estourou = 0;
        while (isalnum(olhar(t)) || olhar(t) == '_') {
            if (n >=TAMANHO_LEXEMA - 1) estourou = 1;
            adicionar_caractere(&token, &n, avancar(t));
        }
        token.tipo = estourou ? TOKEN_ERRO : classificar_palavra(token.lexema);
        return token;
    }

    /* 8. Qualquer outro caractere e invalido */
    adicionar_caractere(&token, &n, avancar(t));
    token.tipo = TOKEN_ERRO;
    return token;
}

static char *ler_arquivo(const char *caminho)
{
    FILE *f = fopen(caminho, "rb");
    if (!f) return NULL;

    size_t capacidade = 1024, tamanho = 0;
    char *buffer = malloc(capacidade);
    if (!buffer) { fclose(f); return NULL; }

    size_t lidos;
    while ((lidos = fread(buffer + tamanho, 1, capacidade - tamanho - 1, f)) > 0) {
        tamanho += lidos;
        if (tamanho + 1 >= capacidade) {
            capacidade *= 2;
            char *novo = realloc(buffer, capacidade);
            if (!novo) { free(buffer); fclose(f); return NULL; }
            buffer = novo;
        }
    }
    buffer[tamanho] = '\0';
    fclose(f);
    return buffer;
}

/*
 * Parser (Analisador Sintatico) .
 *
 * Usa o Analisador Lexico do Tokenizer.

 * Gramatica (palavras reservadas sem diferenciar maiusculas/minusculas):
 *
 *   programa   ::= { comando }  FINAL
 *   comando    ::= create | insert | select
 *
 *   create     ::= CREATE TABLE IDENT '(' coluna { ',' coluna } ')' ';'
 *   coluna     ::= IDENT ( INT | CHAR )
 *
 *   insert     ::= INSERT INTO IDENT VALUES '(' valor { ',' valor } ')' ';'
 *
 *   select     ::= SELECT '*' FROM IDENT [ WHERE condicao { AND condicao } ] ';'
 *   condicao   ::= IDENT operador valor
 *   operador   ::= '=' | '<>' | '!=' | '<' | '<=' | '>' | '>='
 *
 *   valor      ::= NUMERO | STRING
 *
 */
 
/* ------------------------------------------------------------------ */
/* Limites                                                             */
/* ------------------------------------------------------------------ */

#define MAX_COLUNAS   16   /* colunas por tabela                        */
#define MAX_VALORES   16   /* valores por INSERT                        */
#define MAX_CONDICOES 16   /* condicoes por WHERE                       */
#define MAX_MESSAGEM  256  /* tamanho das mensagens de erro             */

/* ------------------------------------------------------------------ */
/* Arvore sintatica (AST)                                              */
/* ------------------------------------------------------------------ */

typedef enum {
    CMD_CREATE,
    CMD_INSERT,
    CMD_SELECT
} TipoComando;

typedef struct {
    char      nome[TAMANHO_LEXEMA];
    TipoToken tipo;                 /* TOKEN_INT ou TOKEN_CHAR              */
} Coluna;

typedef struct {
    TipoToken tipo;                 /* TOKEN_NUMERO ou TOKEN_STRING            */
    int       numero;                  /* valido se tipo == TOKEN_NUMERO        */
    char      str[TAMANHO_LEXEMA];      /* valido se tipo == TOKEN_STRING     */
} Valor;

typedef struct {
    char      coluna[TAMANHO_LEXEMA];
    TipoToken operacao;                   /* TOKEN_IGUAL, TOKEN_MENOR, ...        */
    Valor     valor;
} Condicao;

typedef struct {
    TipoComando tipo;
    char        tabela[TAMANHO_LEXEMA];

    Coluna      colunas[MAX_COLUNAS];       /* CREATE TABLE             */
    int         numero_colunas;

    Valor       valores[MAX_VALORES];       /* INSERT                   */
    int         numero_valores;

    Condicao    condicoes[MAX_CONDICOES];   /* SELECT ... WHERE         */
    int         numero_condicoes;
} Comando;

/* ------------------------------------------------------------------ */
/* Estado do parser                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    Tokenizador lexer;
    Token       atual;              /* token corrente (lookahead)       */
    char        mensagem[MAX_MESSAGEM];       /* ultima mensagem de erro          */
} Parser;

/* Avanca para o proximo token ignorando espacos em branco */
static void avancar_parser(Parser *parser)
{
    do {
        parser->atual = proximo_token(&parser->lexer);
    } while (parser->atual.tipo == TOKEN_ESPACO);
}

static void iniciar_parser(Parser *parser, const char *texto)
{
    iniciar_tokenizador(&parser->lexer, texto);
    parser->mensagem[0] = '\0';
    avancar_parser(parser);
}

/* ------------------------------------------------------------------ */
/* Tratamento de erros                                                 */
/* ------------------------------------------------------------------ */

/* Grava em parser->mensagem: "Linha L, coluna C: <mensagem>". Devolve sempre 0. */
static int erro_formato(Parser *parser, const Token *token, const char *formato, ...)
{
    int numero_linha_coluna = snprintf(parser->mensagem, MAX_MESSAGEM, "Linha %d, coluna %d: ",
                     token->linha, token->coluna);
    if (numero_linha_coluna < 0 || numero_linha_coluna >= MAX_MESSAGEM)
        return 0;

    va_list ap;
    va_start(ap, formato);
    vsnprintf(parser->mensagem + numero_linha_coluna, MAX_MESSAGEM - (size_t)numero_linha_coluna, formato, ap);
    va_end(ap);
    return 0;
}

/* Erro "token inesperado": descreve o que foi encontrado e o que se esperava */
static int falha(Parser *parser, const char *esperado)
{
    const Token *token = &parser->atual;

    switch (token->tipo) {
        case TOKEN_ERRO:
            return erro_formato(parser, token, "token invalido '%s'", token->lexema);
        case TOKEN_NAO_SUPORTADO:
            return erro_formato(parser, token, "construcao nao suportada '%s'", token->lexema);
        case TOKEN_FIM:
            return erro_formato(parser, token, "fim inesperado do texto (esperado: %s)",
                            esperado);
        default:
            return erro_formato(parser, token, "encontrado '%s' (esperado: %s)",
                            token->lexema, esperado);
    }
}

/* Consome o token se for do tipo esperado; caso contrario, gera erro */
static int consumir_token(Parser *parser, TipoToken tipo, const char *descricao,
                    Token *saida)
{
    if (parser->atual.tipo != tipo)
        return falha(parser, descricao);
    if (saida)
        *saida = parser->atual;
    avancar_parser(parser);
    return 1;
}

/* Recuperacao de erro: descarta tokens ate depois do proximo ';' */
static void sincronizar(Parser *parser)
{
    while (parser->atual.tipo != TOKEN_FIM && parser->atual.tipo != TOKEN_PONTO_VIRGULA)
        avancar_parser(parser);
    if (parser->atual.tipo == TOKEN_PONTO_VIRGULA)
        avancar_parser(parser);
}

/* ------------------------------------------------------------------ */
/* Regras gramaticais                                                */
/* ------------------------------------------------------------------ */

static int e_operador(TipoToken t)
{
    switch (t) {
        case TOKEN_IGUAL:
        case TOKEN_DIFERENTE:
        case TOKEN_MENOR:
        case TOKEN_MENOR_IGUAL:
        case TOKEN_MAIOR:
        case TOKEN_MAIOR_IGUAL:
            return 1;
        default:
            return 0;
    }
}

/* valor ::= NUMERO | STRING */
static int parse_valor(Parser *parser, Valor *valor)
{
    Token token = parser->atual;

    if (token.tipo == TOKEN_NUMERO) {
        char *fim;
        errno = 0;
        long x = strtol(token.lexema, &fim, 10);
        if (errno == ERANGE || x > INT_MAX)
            return erro_formato(parser, &token, "numero '%s' fora do intervalo de INT",
                            token.lexema);
        valor->tipo   = TOKEN_NUMERO;
        valor->numero    = (int)x;
        valor->str[0] = '\0';
    } else if (token.tipo == TOKEN_STRING) {
        valor->tipo = TOKEN_STRING;
        valor->numero  = 0;
        strcpy(valor->str, token.lexema);
    } else {
        return falha(parser, "numero ou string");
    }

    avancar_parser(parser);
    return 1;
}

/* CREATE TABLE IDENT '(' coluna { ',' coluna } ')' ';'
 * (o token CREATE ja foi verificado por parse_comando) */
static int parse_create(Parser *parser, Comando *comando)
{
    Token token;

    comando->tipo = CMD_CREATE;
    avancar_parser(parser);                                   /* CREATE */

    if (!consumir_token(parser, TOKEN_TABLE, "TABLE", NULL))  return 0;
    if (!consumir_token(parser, TOKEN_IDENTACAO, "nome da tabela", &token))  return 0;
    strcpy(comando->tabela, token.lexema);
    if (!consumir_token(parser, TOKEN_ABRE_PARENTESE, "'('", NULL)) return 0;

    for (;;) {
        Token  nome;
        Coluna *coluna;

        if (!consumir_token(parser, TOKEN_IDENTACAO, "nome da coluna", &nome))
            return 0;

        if (comando->numero_colunas >= MAX_COLUNAS)
            return erro_formato(parser, &nome, "numero maximo de colunas (%d) excedido",
                            MAX_COLUNAS);

        for (int i = 0; i < comando->numero_colunas; i++)
            if (contexto_igual(comando->colunas[i].nome, nome.lexema))
                return erro_formato(parser, &nome, "coluna '%s' duplicada",
                                nome.lexema);

        coluna = &comando->colunas[comando->numero_colunas];
        strcpy(coluna->nome, nome.lexema);

        if (parser->atual.tipo != TOKEN_INT && parser->atual.tipo != TOKEN_CHAR)
            return falha(parser, "INT ou CHAR");
        coluna->tipo = parser->atual.tipo;
        avancar_parser(parser);
        comando->numero_colunas++;

        if (parser->atual.tipo == TOKEN_VIRGULA) {
            avancar_parser(parser);
            continue;
        }
        break;
    }

    if (!consumir_token(parser, TOKEN_FECHA_PARENTESE, "')' ou ','", NULL)) return 0;
    if (!consumir_token(parser, TOKEN_PONTO_VIRGULA, "';'", NULL)) return 0;
    return 1;
}

/* INSERT INTO IDENT VALUES '(' valor { ',' valor } ')' ';' */
static int parse_insert(Parser *parser, Comando *comando)
{
    Token token;

    comando->tipo = CMD_INSERT;
    avancar_parser(parser);                                   /* INSERT */

    if (!consumir_token(parser, TOKEN_INTO, "INTO", NULL))            return 0;
    if (!consumir_token(parser, TOKEN_IDENTACAO, "nome da tabela", &token))  return 0;
    strcpy(comando->tabela, token.lexema);
    if (!consumir_token(parser, TOKEN_VALUES, "VALUES", NULL))        return 0;
    if (!consumir_token(parser, TOKEN_ABRE_PARENTESE, "'('", NULL))         return 0;

    for (;;) {
        if (comando->numero_valores >= MAX_VALORES)
            return erro_formato(parser, &parser->atual,
                            "numero maximo de valores (%d) excedido",
                            MAX_VALORES);

        if (!parse_valor(parser, &comando->valores[comando->numero_valores]))
            return 0;
        comando->numero_valores++;

        if (parser->atual.tipo == TOKEN_VIRGULA) {
            avancar_parser(parser);
            continue;
        }
        break;
    }

    if (!consumir_token(parser, TOKEN_FECHA_PARENTESE, "')' ou ','", NULL))     return 0;
    if (!consumir_token(parser, TOKEN_PONTO_VIRGULA, "';'", NULL))        return 0;
    return 1;
}

/* condicao ::= IDENT operador valor */
static int parse_condicao(Parser *parser, Condicao *condicao)
{
    Token token;

    if (!consumir_token(parser, TOKEN_IDENTACAO, "nome da coluna", &token))
        return 0;
    strcpy(condicao->coluna, token.lexema);

    if (!e_operador(parser->atual.tipo))
        return falha(parser, "operador (=, <>, !=, <, <=, >, >=)");
    condicao->operacao = parser->atual.tipo;
    avancar_parser(parser);

    return parse_valor(parser, &condicao->valor);
}

/* SELECT '*' FROM IDENT [ WHERE condicao { AND condicao } ] ';' */
static int parse_select(Parser *parser, Comando *comando)
{
    Token token;

    comando->tipo = CMD_SELECT;
    avancar_parser(parser);                                   /* SELECT */

    if (!consumir_token(parser, TOKEN_ASTERISCO, "'*'", NULL))        return 0;
    if (!consumir_token(parser, TOKEN_FROM, "FROM", NULL))            return 0;
    if (!consumir_token(parser, TOKEN_IDENTACAO, "nome da tabela", &token))  return 0;
    strcpy(comando->tabela, token.lexema);

    if (parser->atual.tipo == TOKEN_WHERE) {
        avancar_parser(parser);

        for (;;) {
            if (comando->numero_condicoes >= MAX_CONDICOES)
                return erro_formato(parser, &parser->atual,
                                "numero maximo de condicoes (%d) excedido",
                                MAX_CONDICOES);

            if (!parse_condicao(parser, &comando->condicoes[comando->numero_condicoes]))
                return 0;
            comando->numero_condicoes++;

            if (parser->atual.tipo == TOKEN_AND) {
                avancar_parser(parser);
                continue;
            }
            break;
        }
    }

    if (!consumir_token(parser, TOKEN_PONTO_VIRGULA, "';', WHERE ou AND", NULL))
        return 0;
    return 1;
}

/* comando ::= create | insert | select */
static int parse_comando(Parser *parser, Comando *comando)
{
    memset(comando, 0, sizeof *comando);

    switch (parser->atual.tipo) {
        case TOKEN_CREATE: return parse_create(parser, comando);
        case TOKEN_INSERT: return parse_insert(parser, comando);
        case TOKEN_SELECT: return parse_select(parser, comando);
        default:         return falha(parser, "CREATE, INSERT ou SELECT");
    }
}

/* ------------------------------------------------------------------ */
/* Impressao da AST                                                    */
/* ------------------------------------------------------------------ */

static const char *simbolo_operacao(TipoToken operacao)
{
    switch (operacao) {
        case TOKEN_IGUAL:       return "=";
        case TOKEN_DIFERENTE:   return "<>";
        case TOKEN_MENOR:       return "<";
        case TOKEN_MENOR_IGUAL: return "<=";
        case TOKEN_MAIOR:       return ">";
        case TOKEN_MAIOR_IGUAL: return ">=";
        default:              return "?";
    }
}

static void imprimir_valor(const Valor *valor)
{
    if (valor->tipo == TOKEN_NUMERO)
        printf("%d", valor->numero);
    else
        printf("'%s'", valor->str);
}

static void imprimir_comando(const Comando *comando)
{
    switch (comando->tipo) {
        case CMD_CREATE:
            printf("CREATE TABLE\n");
            printf("  tabela : %s\n", comando->tabela);
            printf("  colunas:\n");
            for (int i = 0; i < comando->numero_colunas; i++)
                printf("    - %s : %s\n", comando->colunas[i].nome,
                       nome_token(comando->colunas[i].tipo));
            break;

        case CMD_INSERT:
            printf("INSERT\n");
            printf("  tabela : %s\n", comando->tabela);
            printf("  valores: ");
            for (int i = 0; i < comando->numero_valores; i++) {
                if (i) printf(", ");
                imprimir_valor(&comando->valores[i]);
            }
            printf("\n");
            break;

        case CMD_SELECT:
            printf("SELECT\n");
            printf("  tabela : %s\n", comando->tabela);
            if (comando->numero_condicoes == 0) {
                printf("  where  : (nenhum)\n");
            } else {
                printf("  where  :\n");
                for (int i = 0; i < comando->numero_condicoes; i++) {
                    printf("    %s %s %s", i ? "AND" : "   ",
                           comando->condicoes[i].coluna,
                           simbolo_operacao(comando->condicoes[i].operacao));
                    printf(" ");
                    imprimir_valor(&comando->condicoes[i].valor);
                    printf("\n");
                }
            }
            break;
    }
}

#ifndef DB_CRAFT_MAIN
int main(void) {

    printf("\n--- Alocacao de Paginas ---\n");
    
    int pagina_alvo = 2;
    int slot_alvo = 0;

    /* Garante que as paginas 0, 1 e 2 existam antes de usar a pagina 2 */
    while (total_paginas_alocadas() <= pagina_alvo) {
        if (aloca_pagina() < 0) {
            fprintf(stderr, "Erro Ao Alocar Paginas Necessarias.\n");
            return 1;
        }
    }

    unsigned char buffer[TAMANHO_PAGINA];
    unsigned char registro[TAMANHO_REGISTRO] = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08
    };

    /* Carrega a página atual (para não sobrescrever o restante do conteúdo) */
    if (ler_pagina(pagina_alvo, buffer, sizeof(buffer)) != 0) {
        return 1;
    }

    /* Calcula o deslocamento do slot dentro da página, após o cabeçalho */
    int offset_na_pagina = TAMANHO_CABECALHO + slot_alvo * TAMANHO_REGISTRO;

    /* Copia o registro para a posição do slot dentro do buffer da página */
    memcpy(buffer + offset_na_pagina, registro, TAMANHO_REGISTRO);

    /* Grava a página de volta no arquivo (já sincroniza com o disco) */
    if (escreve_pagina(pagina_alvo, buffer, sizeof(buffer)) != 0) {
        return 1;
    }

    /* Sincronização explícita adicional, garantindo persistência total */
    sincroniza_dados();

    /* Calcula o byte absoluto no arquivo onde o registro foi gravado */
    long byte_absoluto = (long)pagina_alvo * TAMANHO_PAGINA + offset_na_pagina;

    printf("O Registro Começa No Byte: %ld\n", byte_absoluto);

    /* Serializa e desserializa um registro */

    Registro registro_bruto = { .id = 1, .valor = 20260001 };
    
    unsigned char registro_bytes[TAMANHO_REGISTRO];
    
    serializa_registro(&registro_bruto,registro_bytes);

    unsigned char pagina_lida[TAMANHO_PAGINA];
    
    Registro registro_lido;
    
    ler_pagina(pagina_alvo, pagina_lida, sizeof(pagina_lida));
    
    desserializa_registro(pagina_lida + offset_na_pagina, &registro_lido);
    
    printf("Registro Relido Do Disco e Desserializado: ID=%d, Valor=%d\n",
           registro_lido.id, registro_lido.valor);


     printf("\n ----- Cache de Paginas ----- \n");

    /* Garante paginas suficientes para forcar expulsao (mais paginas que frames) */
    while (total_paginas_alocadas() < NUM_FRAMES + 2) {
        if (aloca_pagina() < 0) {
            fprintf(stderr, "Erro Ao Alocar Paginas Extras Para O Teste Do Cache.\n");
            return 1;
        }
    }

    /* fixa(2): primeira vez -> falta (miss) */
    unsigned char *buf2 = fixa(pagina_alvo);
    if (buf2 == NULL) return 1;

    Registro lido_via_cache;
    
    desserializa_registro(buf2 + offset_na_pagina, &lido_via_cache);
    
    printf("[CACHE] Pagina %d Fixada -> ID=%d, Valor=%d\n",
           pagina_alvo, lido_via_cache.id, lido_via_cache.valor);

    /* Modifica o registro diretamente no buffer do cache e marca a pagina como suja */
    
    Registro atualizado = { .id = 99, .valor = 12345 };
    
    unsigned char atualizado_bytes[TAMANHO_REGISTRO];
    
    serializa_registro(&atualizado, atualizado_bytes);
    
    memcpy(buf2 + offset_na_pagina, atualizado_bytes, TAMANHO_REGISTRO);

    solta(pagina_alvo, 1); /* solta marcando suja; NO-FORCE: nao grava ainda */

    /* fixa(2) novamente: deve ser acerto (hit) e devolver a MESMA referencia */
    unsigned char *buf2_de_novo = fixa(pagina_alvo);
    
    printf("[CACHE] Segunda Fixacao Da Pagina %d -> Mesma Referencia? %s\n",
           pagina_alvo, (buf2 == buf2_de_novo) ? "Sim" : "Nao");
    
           solta(pagina_alvo, 0);

    /* Fixa e solta outras paginas para exceder a capacidade do cache e forcar
    expulsoes via LRU, exercitando a politica STEAL sobre a pagina suja acima */
    
    for (int p = 0; p < NUM_FRAMES + 2; p++) {
        if (p == pagina_alvo) continue;
        unsigned char *bp = fixa(p);
        if (bp != NULL) {
            solta(p, 0);
        }
    }

    /* Mesmo que a pagina 2 tenha sido expulsa pelo LRU nesse meio tempo, a
    politica STEAL garante que ela foi gravada em disco antes da expulsao */
    
    unsigned char pagina_verificacao[TAMANHO_PAGINA];
    
    ler_pagina(pagina_alvo, pagina_verificacao, sizeof(pagina_verificacao));
    
    Registro verificado;
    
    desserializa_registro(pagina_verificacao + offset_na_pagina, &verificado);
    
    printf("[CACHE] Apos Possivel Expulsao, Disco Contem ID=%d, Valor=%d (Esperado ID=99, Valor=12345)\n",
           verificado.id, verificado.valor);

    /* descarrega() explicito: garante que qualquer pagina ainda suja no cache
       seja persistida (uso tipico ao final do programa) */
    for (int p = 0; p < total_paginas_alocadas(); p++) {
        descarrega(p);
    }
    
    printf("\n ---- Arvore B+ -----\n");

    /*
    
    Limpa o banco pra evitar stack overflow
    
    A funcao inserir_registro() causaria recursao infinita devido a pagina 0 se tornar um no e nao a raiz

    A pagina 0 se tornaria um no devido a alocacao e o cache de paginas criarem 6 paginas

    Como o arquivo ficaria com 6 paginas,a criacao da raiz e dos metadados da arvore nao ocorre

    Como o desserializa_no() ler o no zerado (raiz = 0 ),o codigo desceria pra no.filhos[0] e entraria em loop infinito ate
    estourar a pilha

    A pagina 0 da arvore deve ser torna os metadados e a pagina 1 a raiz 

    Pra isso,a Arvore B + precisa ser criada antes de qualquer bloco poluir o arquivo com paginas vazias

    Logo o remove() limpa o arquivo e evita a possivel falha de segmentacao

    */
    remove(ARQUIVO_BD);

    if (inicializa_arvore() != 0) {
        fprintf(stderr, "Erro Ao Inicializar A B+ Tree.\n");
        return 1;
    }

    int32_t chaves[] = { 35, 45, 50, 55 };
    
    int n = (int)(sizeof(chaves) / sizeof(chaves[0]));

    printf("\n ---- Insercoes na B+ Tree (MAXIMO: %d CHAVES POR NO) ----- \n", MAX_CHAVES);
    
    for (int i = 0; i < n; i++) {
        Registro reg = { .id = chaves[i], .valor = chaves[i] * 10 };
        int r = inserir_valor(&reg);
        if (r == INSERE_DUPLICADO) {
            printf("\n Chave %d Ja Existe.\n", chaves[i]);
            continue;
        }
        if (r != INSERE_OK) {
            fprintf(stderr, "Erro Ao Inserir a Chave %d\n", chaves[i]);
            return 1;
        }
        printf("\n Apos Inserir %d:\n", chaves[i]);
        imprimir_arvore();
    }

    printf("\n ------ Busca B + Tree --------\n");
    
    int32_t buscas[] = { 35, 50, 55, 40 };
    
    for (int i = 0; i < 4; i++) {
        Registro achado;
        int r = buscar_valor(buscas[i], &achado);
        if (r == 0) {
            printf("Busca(%d): Encontrado -> ID=%d, Valor=%d\n", buscas[i], achado.id, achado.valor);
        } else if (r == 1) {
            printf("Busca(%d): Nao Encontrado\n", buscas[i]);
        } else {
            printf("Busca(%d): Erro\n", buscas[i]);
        }
    }

    /* Persiste todas as paginas ainda sujas no cache (NO-FORCE) */

    for (int p = 0; p < total_paginas_alocadas(); p++) {
        descarrega(p);
    }

    sincroniza_dados();
    
    printf("\n ----- TOKENIZADOR ---- \n\n");
    
    static const char *EXEMPLO =
      "CREATE TABLE alunos (id INT, nome CHAR);\n"
      "INSERT INTO alunos VALUES (1, 'Ana');\n"
      "SELECT * FROM alunos WHERE id >= 1 AND nome <> 'Bia';";

    char *arquivo = NULL;
    const char *texto = EXEMPLO;

    Tokenizador t;
    iniciar_tokenizador(&t, texto);

    Parser parser;
    iniciar_parser(&parser, texto);

    printf("%-4s %-14s %-8s %s\n", "#", "TOKEN", "POSICAO", "LEXEMA");
    printf("\n --------------------------------------------------\n");

    int ordem = 0;
    int status = 0;
    int erros =0 ;
    
    
    Token token;
    do {
        token = proximo_token(&t);
        printf("%-4d %-14s %d:%-5d '%s'\n",
               ++ordem, nome_token(token.tipo), token.linha, token.coluna, token.lexema);

        if (token.tipo == TOKEN_ERRO || token.tipo == TOKEN_NAO_SUPORTADO)
            status = 1;
    } while (token.tipo != TOKEN_FIM);
    
    printf("\n ------- PARSER ------ \n\n");
    
    while (parser.atual.tipo != TOKEN_FIM) {
        Comando comando;

        if (parse_comando(&parser, &comando)) {
            printf("[COMANDO %d] ", ++ordem);
            imprimir_comando(&comando);
        } else {
            printf("[COMANDO %d] Erro Sintatico\n  %s\n", ++ordem, parser.mensagem);
            erros++;
            sincronizar(&parser);
        }
        printf("\n");
    }

    if (erros)
        printf("Analise Concluida Com %d Erro(s)\n", erros);
    else
        printf("Analise Concluida Sem Erros\n");

    free(arquivo);
    
    return status;
    
    return erros ? 1 : 0;
    
    return 0;
}
#endif /*DB_CRAFT_MAIN */
