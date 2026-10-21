#include <stdio.h>
#include <stdlib.h>
#include<stdint.h>
#include <string.h>
#include <unistd.h> // fsync - Gravação em Disco 
#include<fcntl.h> // struct flock - Isolamento de Páginas

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

#ifndef DB_CRAFT_TEST_MAIN
int main(void) {

    printf("\n--- Alocacao de Paginas ---\n");
    
    int pagina_alvo = 2;
    int slot_alvo = 0;

    /* Garante que as paginas 0, 1 e 2 existam antes de usar a pagina 2 */
    while (total_paginas_alocadas() <= pagina_alvo) {
        if (aloca_pagina() < 0) {
            fprintf(stderr, "Erro ao alocar paginas necessarias.\n");
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

    printf("O registro começa no byte: %ld\n", byte_absoluto);

    /* Serializa e desserializa um registro */

    Registro registro_bruto = { .id = 1, .valor = 20260001 };
    
    unsigned char registro_bytes[TAMANHO_REGISTRO];
    
    serializa_registro(&registro_bruto,registro_bytes);

    unsigned char pagina_lida[TAMANHO_PAGINA];
    
    Registro registro_lido;
    
    ler_pagina(pagina_alvo, pagina_lida, sizeof(pagina_lida));
    
    desserializa_registro(pagina_lida + offset_na_pagina, &registro_lido);
    
    printf("Registro relido do disco e desserializado: id=%d, valor=%d\n",
           registro_lido.id, registro_lido.valor);


     printf("\n---Cache de Paginas ---\n");

    /* Garante paginas suficientes para forcar expulsao (mais paginas que frames) */
    while (total_paginas_alocadas() < NUM_FRAMES + 2) {
        if (aloca_pagina() < 0) {
            fprintf(stderr, "Erro ao alocar paginas extras para o teste do cache.\n");
            return 1;
        }
    }

    /* fixa(2): primeira vez -> falta (miss) */
    unsigned char *buf2 = fixa(pagina_alvo);
    if (buf2 == NULL) return 1;

    Registro lido_via_cache;
    
    desserializa_registro(buf2 + offset_na_pagina, &lido_via_cache);
    
    printf("[cache] pagina %d fixada -> id=%d, valor=%d\n",
           pagina_alvo, lido_via_cache.id, lido_via_cache.valor);

    /* Modifica o registro diretamente no buffer do cache e marca a pagina como suja */
    
    Registro atualizado = { .id = 99, .valor = 12345 };
    
    unsigned char atualizado_bytes[TAMANHO_REGISTRO];
    
    serializa_registro(&atualizado, atualizado_bytes);
    
    memcpy(buf2 + offset_na_pagina, atualizado_bytes, TAMANHO_REGISTRO);

    solta(pagina_alvo, 1); /* solta marcando suja; NO-FORCE: nao grava ainda */

    /* fixa(2) novamente: deve ser acerto (hit) e devolver a MESMA referencia */
    unsigned char *buf2_de_novo = fixa(pagina_alvo);
    
    printf("[cache] segunda fixacao da pagina %d -> mesma referencia? %s\n",
           pagina_alvo, (buf2 == buf2_de_novo) ? "sim" : "nao");
    
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
    
    printf("[cache] apos possivel expulsao, disco contem id=%d, valor=%d (esperado id=99, valor=12345)\n",
           verificado.id, verificado.valor);

    /* descarrega() explicito: garante que qualquer pagina ainda suja no cache
       seja persistida (uso tipico ao final do programa) */
    for (int p = 0; p < total_paginas_alocadas(); p++) {
        descarrega(p);
    }
    
    printf("\n-- Arvore B + ---\n");

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
        fprintf(stderr, "Erro ao inicializar a arvore B+.\n");
        return 1;
    }

    int32_t chaves[] = { 35, 45, 50, 55 };
    
    int n = (int)(sizeof(chaves) / sizeof(chaves[0]));

    printf("=== Insercoes na Arvore B+ (maximo de %d chaves por no) ===\n", MAX_CHAVES);
    
    for (int i = 0; i < n; i++) {
        Registro reg = { .id = chaves[i], .valor = chaves[i] * 10 };
        int r = inserir_valor(&reg);
        if (r == INSERE_DUPLICADO) {
            printf("\nChave %d ja existe.\n", chaves[i]);
            continue;
        }
        if (r != INSERE_OK) {
            fprintf(stderr, "Erro ao inserir a chave %d\n", chaves[i]);
            return 1;
        }
        printf("\n Apos inserir %d:\n", chaves[i]);
        imprimir_arvore();
    }

    printf("\n=== Busca ===\n");
    
    int32_t buscas[] = { 35, 50, 55, 40 };
    
    for (int i = 0; i < 4; i++) {
        Registro achado;
        int r = buscar_valor(buscas[i], &achado);
        if (r == 0) {
            printf("busca(%d): encontrado -> id=%d, valor=%d\n", buscas[i], achado.id, achado.valor);
        } else if (r == 1) {
            printf("busca(%d): nao encontrado\n", buscas[i]);
        } else {
            printf("busca(%d): erro\n", buscas[i]);
        }
    }

    /* Persiste todas as paginas ainda sujas no cache (NO-FORCE) */

    for (int p = 0; p < total_paginas_alocadas(); p++) {
        descarrega(p);
    }

    sincroniza_dados();
    
    return 0;
}
#endif /* DB_CRAFT_TEST_MAIN */
