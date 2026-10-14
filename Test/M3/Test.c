/*
 * Testes da Arvore B+ implementada em M3.c

 * Variacoes uteis (o cache e a arvore devem funcionar com qualquer tamanho):
 *     gcc -DNUM_FRAMES=1  -o M3_Test M3_Test.c
 *     gcc -DNUM_FRAMES=64 -o M3_Test M3_Test.c
 *
 * Este arquivo faz #include "M3.c" para ter acesso as funcoes static
 * (ler_no, ler_raiz, serializar_no, ...) e ao estado do cache, o que permite
 * auditar a estrutura interna da arvore e nao apenas o comportamento externo.
 *
 * Os testes rodam dentro de um diretorio temporario (mkdtemp + chdir), entao
 * o arquivo "craft-db.dat" real nunca e tocado.
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <stdarg.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#define CRAFT_DB_TEST_BUILD

#include "M3.c"

/* ======================================================================
 * Mini framework de testes
 * ====================================================================== */

static int testes_executados   = 0;
static int testes_falhos       = 0;
static long verificacoes       = 0;
static int falhas_teste_atual  = 0;

#define VERIFICA(cond) do {                                                  \
    verificacoes++;                                                          \
    if (!(cond)) {                                                           \
        falhas_teste_atual++;                                                \
        printf("    -> linha %d: falhou: %s\n", __LINE__, #cond);            \
    }                                                                        \
} while (0)

#define VERIFICA_IGUAL(esperado, obtido) do {                                \
    long _e = (long)(esperado), _o = (long)(obtido);                         \
    verificacoes++;                                                          \
    if (_e != _o) {                                                          \
        falhas_teste_atual++;                                                \
        printf("    -> linha %d: %s: esperado %ld, obtido %ld\n",            \
               __LINE__, #obtido, _e, _o);                                   \
    }                                                                        \
} while (0)

/* Como VERIFICA, mas interrompe o teste atual se falhar. */
#define EXIGE(cond) do {                                                     \
    verificacoes++;                                                          \
    if (!(cond)) {                                                           \
        falhas_teste_atual++;                                                \
        printf("    -> linha %d: pre-condicao falhou: %s\n",                 \
               __LINE__, #cond);                                             \
        return;                                                              \
    }                                                                        \
} while (0)

/* ======================================================================
 * Utilitarios
 * ====================================================================== */

#define SEMENTE 2463534242u
static uint32_t rng_estado = SEMENTE;

static uint32_t proximo_aleatorio(void) {   /* xorshift32: deterministico */
    uint32_t x = rng_estado;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_estado = x;
    return x;
}

static void embaralha(int32_t *v, int n) {
    for (int i = n - 1; i > 0; i--) {
        int j = (int)(proximo_aleatorio() % (uint32_t)(i + 1));
        int32_t t = v[i]; v[i] = v[j]; v[j] = t;
    }
}

static int cmp_int32(const void *a, const void *b) {
    int32_t x = *(const int32_t *)a, y = *(const int32_t *)b;
    return (x > y) - (x < y);
}

/* Valor "esperado" associado a cada id (diferente do id, para detectar
 * trocas entre campos). Usa aritmetica sem sinal para evitar overflow UB. */
static int32_t valor_de(int32_t id) {
    return (int32_t)((uint32_t)id * 31u + 7u);
}

static int insere(int32_t id) {
    Registro r;
    r.id = id;
    r.valor = valor_de(id);
    return inserir_valor(&r);
}

/* 1 se a chave existe e o registro retornado esta integro. */
static int busca_ok(int32_t id) {
    Registro r;
    memset(&r, 0, sizeof(r));
    if (buscar_valor(id, &r) != 0) return 0;
    return r.id == id && r.valor == valor_de(id);
}

static int busca_ausente(int32_t id) {
    Registro r;
    r.id = 12345; r.valor = 6789;
    return buscar_valor(id, &r) == 1;
}

static int conta_nao_encontradas(const int32_t *v, int n) {
    int c = 0;
    for (int i = 0; i < n; i++) if (!busca_ok(v[i])) c++;
    return c;
}

static int altura_arvore(void) {
    int pagina = ler_raiz();
    int h = 0;
    No no;
    for (;;) {
        if (pagina < 0 || ler_no(pagina, &no) != 0) return -1;
        h++;
        if (no.tipo == NO_FOLHA) return h;
        pagina = no.filhos[0];
        if (h > 64) return -1;
    }
}

/* ---- controle do ambiente ---------------------------------------- */

static void reinicia_ambiente(void) {
    unlink(ARQUIVO_BD);
    cache_inicializado = 0;       /* descarta qualquer frame (inclusive sujo) */
    inicializa_cache();
    contador_leituras_disco = 0;
}

static int descarrega_tudo(void) {
    for (int i = 0; i < NUM_FRAMES; i++) {
        if (cache.frames[i].pagina != -1 &&
            descarrega(cache.frames[i].pagina) != 0) {
            return -1;
        }
    }
    return 0;
}

/* Simula "fechar e reabrir o programa": grava o que esta sujo e zera o cache. */
static int reinicia_cache_mantendo_disco(void) {
    if (descarrega_tudo() != 0) return -1;
    cache_inicializado = 0;
    inicializa_cache();
    return 0;
}

static int fixacoes_pendentes(void) {
    int total = 0;
    for (int i = 0; i < NUM_FRAMES; i++) {
        if (cache.frames[i].pagina != -1) total += cache.frames[i].fixacoes;
    }
    return total;
}

static int stderr_salvo = -1;
static void silencia_stderr(void) {
    fflush(stderr);
    stderr_salvo = dup(STDERR_FILENO);
    int nulo = open("/dev/null", O_WRONLY);
    dup2(nulo, STDERR_FILENO);
    close(nulo);
}
static void restaura_stderr(void) {
    fflush(stderr);
    dup2(stderr_salvo, STDERR_FILENO);
    close(stderr_salvo);
}

/* ======================================================================
 * Auditoria estrutural da arvore
 *
 * Verifica, percorrendo a arvore inteira a partir da raiz:
 *   - tipo/numero_chaves validos no disco (o desserializador mascara lixo)
 *   - chaves estritamente crescentes dentro de cada no
 *   - cada chave de uma subarvore respeita os separadores do pai
 *   - todas as folhas estao na mesma profundidade
 *   - ocupacao minima (folha nao-raiz >= 2 chaves; interno >= 1 chave)
 *   - nenhuma pagina visitada duas vezes (sem ciclos/compartilhamento)
 *   - lista encadeada de folhas (proxima) = ordem da travessia em profundidade
 *   - nenhuma pagina "vazada": total = 1 (meta) + folhas + internos
 * ====================================================================== */

#define MAX_FOLHAS_AUD   8192
#define MAX_PAGINAS_AUD  32768

typedef struct {
    int  ok;
    int  altura;
    long total_chaves;
    int  n_folhas;
    int  n_internos;
    int  altura_folha;
    int  folhas[MAX_FOLHAS_AUD];
    char erro[200];
} Auditoria;

static Auditoria aud;
static char visitada[MAX_PAGINAS_AUD];

static int audita_erro(const char *fmt, ...) {
    if (aud.ok) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(aud.erro, sizeof(aud.erro), fmt, ap);
        va_end(ap);
    }
    aud.ok = 0;
    return -1;
}

static int audita_no(int pagina, int tem_lo, int32_t lo, int tem_hi, int32_t hi,
                     int prof, int eh_raiz) {
    if (prof > 64)
        return audita_erro("profundidade excessiva (ciclo?) na pagina %d", pagina);
    if (pagina < 1 || pagina >= total_paginas_alocadas() || pagina >= MAX_PAGINAS_AUD)
        return audita_erro("ponteiro de pagina invalido: %d", pagina);
    if (visitada[pagina])
        return audita_erro("pagina %d alcancada mais de uma vez", pagina);
    visitada[pagina] = 1;

    /* leitura "crua" para detectar lixo que desserializar_no esconderia */
    unsigned char *bruto = fixa(pagina);
    if (bruto == NULL) return audita_erro("nao foi possivel ler a pagina %d", pagina);
    int32_t tipo_bruto = (int32_t)desserializa_uint32_le(bruto);
    int32_t nc_bruto   = (int32_t)desserializa_uint32_le(bruto + 4);
    solta(pagina, 0);
    if (tipo_bruto != NO_FOLHA && tipo_bruto != NO_INTERNO)
        return audita_erro("pagina %d com tipo invalido (%d)", pagina, tipo_bruto);
    if (nc_bruto < 0 || nc_bruto > MAX_CHAVES)
        return audita_erro("pagina %d com numero_chaves invalido no disco (%d)", pagina, nc_bruto);

    No no;
    if (ler_no(pagina, &no) != 0) return audita_erro("falha ao ler no %d", pagina);

    if (no.tipo == NO_FOLHA) {
        if (aud.altura_folha == -1) aud.altura_folha = prof;
        else if (aud.altura_folha != prof)
            return audita_erro("folha %d na profundidade %d, esperado %d",
                               pagina, prof, aud.altura_folha);

        int minimo = eh_raiz ? 0 : 2;
        if (no.numero_chaves < minimo)
            return audita_erro("folha %d com %d chaves (minimo %d)", pagina, no.numero_chaves, minimo);

        for (int i = 0; i < no.numero_chaves; i++) {
            int32_t id = no.registros[i].id;
            if (i > 0 && id <= no.registros[i - 1].id)
                return audita_erro("folha %d: chaves fora de ordem/duplicadas (%d apos %d)",
                                   pagina, id, no.registros[i - 1].id);
            if (tem_lo && id < lo)
                return audita_erro("folha %d: chave %d < limite inferior %d", pagina, id, lo);
            if (tem_hi && id >= hi)
                return audita_erro("folha %d: chave %d >= limite superior %d", pagina, id, hi);
        }
        aud.total_chaves += no.numero_chaves;
        if (aud.n_folhas >= MAX_FOLHAS_AUD) return audita_erro("MAX_FOLHAS_AUD excedido");
        aud.folhas[aud.n_folhas++] = pagina;
        return 0;
    }

    /* no interno */
    aud.n_internos++;
    if (no.numero_chaves < 1)
        return audita_erro("no interno %d sem chaves", pagina);

    for (int i = 0; i < no.numero_chaves; i++) {
        if (i > 0 && no.chaves[i] <= no.chaves[i - 1])
            return audita_erro("interno %d: separadores fora de ordem", pagina);
        if (tem_lo && no.chaves[i] < lo)
            return audita_erro("interno %d: separador %d < limite inferior %d", pagina, no.chaves[i], lo);
        if (tem_hi && no.chaves[i] >= hi)
            return audita_erro("interno %d: separador %d >= limite superior %d", pagina, no.chaves[i], hi);
    }
    for (int i = 0; i <= no.numero_chaves; i++) {
        int f_tem_lo = (i == 0) ? tem_lo : 1;
        int32_t f_lo = (i == 0) ? lo : no.chaves[i - 1];
        int f_tem_hi = (i == no.numero_chaves) ? tem_hi : 1;
        int32_t f_hi = (i == no.numero_chaves) ? hi : no.chaves[i];
        if (audita_no(no.filhos[i], f_tem_lo, f_lo, f_tem_hi, f_hi, prof + 1, 0) != 0)
            return -1;
    }
    return 0;
}

static int audita_arvore(void) {
    memset(&aud, 0, sizeof(aud));
    memset(visitada, 0, sizeof(visitada));
    aud.ok = 1;
    aud.altura_folha = -1;

    int raiz = ler_raiz();
    if (raiz < 1) { audita_erro("ponteiro de raiz invalido: %d", raiz); return 0; }

    if (audita_no(raiz, 0, 0, 0, 0, 0, 1) != 0) return 0;

    for (int i = 0; i < aud.n_folhas; i++) {
        No f;
        if (ler_no(aud.folhas[i], &f) != 0) { audita_erro("falha ao reler folha"); return 0; }
        int esperado = (i + 1 < aud.n_folhas) ? aud.folhas[i + 1] : SEM_PAGINA;
        if (f.proxima != esperado) {
            audita_erro("folha %d: proxima=%d, esperado %d (encadeamento quebrado)",
                        aud.folhas[i], f.proxima, esperado);
            return 0;
        }
    }

    int total_esperado = 1 + aud.n_folhas + aud.n_internos;
    if (total_paginas_alocadas() != total_esperado) {
        audita_erro("paginas alocadas=%d, mas a arvore usa %d (vazamento de paginas)",
                    total_paginas_alocadas(), total_esperado);
        return 0;
    }

    aud.altura = aud.altura_folha + 1;
    return 1;
}

#define VERIFICA_ARVORE() do {                                               \
    int _ok = audita_arvore();                                               \
    verificacoes++;                                                          \
    if (!_ok) {                                                              \
        falhas_teste_atual++;                                                \
        printf("    -> linha %d: invariante da arvore violada: %s\n",        \
               __LINE__, aud.erro);                                          \
    }                                                                        \
} while (0)

#define VERIFICA_ARVORE_COM(n) do {                                          \
    VERIFICA_ARVORE();                                                       \
    VERIFICA_IGUAL((n), aud.total_chaves);                                   \
} while (0)

/* Coleta as chaves seguindo a lista encadeada de folhas (da menor para a maior). */
static int coleta_chaves(int32_t *saida, int max) {
    int pagina = ler_raiz();
    No no;
    int passos = 0;
    for (;;) {
        if (ler_no(pagina, &no) != 0) return -1;
        if (no.tipo == NO_FOLHA) break;
        pagina = no.filhos[0];
        if (++passos > 64) return -1;
    }
    int n = 0;
    passos = 0;
    for (;;) {
        for (int i = 0; i < no.numero_chaves; i++) {
            if (n >= max) return -1;
            saida[n++] = no.registros[i].id;
        }
        if (no.proxima == SEM_PAGINA) break;
        if (++passos > MAX_FOLHAS_AUD) return -1;
        if (ler_no(no.proxima, &no) != 0) return -1;
    }
    return n;
}

static int prepara(void) {
    reinicia_ambiente();
    return inicializar_arvore();
}

#define N_GRANDE 300
static int32_t chaves[4096];
static int32_t coletadas[4096];

/* ======================================================================
 * 1) inicializar_arvore
 * ====================================================================== */

static void teste_inicializa_banco_vazio(void) {
    VERIFICA_IGUAL(0, total_paginas_alocadas());
    VERIFICA_IGUAL(0, inicializar_arvore());

    /* pagina 0 = meta, pagina 1 = raiz (folha vazia) */
    VERIFICA_IGUAL(2, total_paginas_alocadas());
    VERIFICA_IGUAL(1, ler_raiz());

    No raiz;
    EXIGE(ler_no(1, &raiz) == 0);
    VERIFICA_IGUAL(NO_FOLHA, raiz.tipo);
    VERIFICA_IGUAL(0, raiz.numero_chaves);
    VERIFICA_IGUAL(SEM_PAGINA, raiz.proxima);
    VERIFICA_ARVORE_COM(0);
    VERIFICA_IGUAL(1, aud.altura);
}

static void teste_inicializa_e_idempotente(void) {
    EXIGE(inicializar_arvore() == 0);
    VERIFICA_IGUAL(0, inicializar_arvore());
    VERIFICA_IGUAL(0, inicializar_arvore());
    VERIFICA_IGUAL(2, total_paginas_alocadas());

    /* chamar de novo com dados ja inseridos nao pode apagar nada */
    for (int i = 1; i <= 20; i++) EXIGE(insere(i) == INSERE_OK);
    int paginas = total_paginas_alocadas();
    int raiz = ler_raiz();

    VERIFICA_IGUAL(0, inicializar_arvore());
    VERIFICA_IGUAL(paginas, total_paginas_alocadas());
    VERIFICA_IGUAL(raiz, ler_raiz());
    for (int i = 1; i <= 20; i++) VERIFICA(busca_ok(i));
    VERIFICA_ARVORE_COM(20);
}

static void teste_banco_nao_inicializado(void) {
    /* sem inicializar_arvore(): as operacaoeracoes devem falhar de forma controlada */
    silencia_stderr();
    int r_ins = insere(1);
    Registro r;
    int r_bus = buscar_valor(1, &r);
    restaura_stderr();

    VERIFICA_IGUAL(INSERE_ERRO, r_ins);
    VERIFICA_IGUAL(-1, r_bus);

    /* e a arvore continua inicializavel depois disso */
    VERIFICA_IGUAL(0, inicializar_arvore());
    VERIFICA_IGUAL(INSERE_OK, insere(1));
    VERIFICA(busca_ok(1));
}

/* ======================================================================
 * 2) buscar_valor
 * ====================================================================== */

static void teste_busca_em_arvore_vazia(void) {
    EXIGE(inicializar_arvore() == 0);
    Registro r = { 111, 222 };
    VERIFICA_IGUAL(1, buscar_valor(42, &r));
    VERIFICA_IGUAL(1, buscar_valor(0, &r));
    VERIFICA_IGUAL(1, buscar_valor(-7, &r));
    VERIFICA_IGUAL(1, buscar_valor(42, NULL));
    /* busca sem sucesso nao deve alterar a saida */
    VERIFICA_IGUAL(111, r.id);
    VERIFICA_IGUAL(222, r.valor);
}

static void teste_insere_e_busca_uma_chave(void) {
    EXIGE(inicializar_arvore() == 0);
    Registro novo = { 10, 12345 };
    VERIFICA_IGUAL(INSERE_OK, inserir_valor(&novo));

    Registro lido = { 0, 0 };
    VERIFICA_IGUAL(0, buscar_valor(10, &lido));
    VERIFICA_IGUAL(10, lido.id);
    VERIFICA_IGUAL(12345, lido.valor);

    VERIFICA_IGUAL(2, total_paginas_alocadas());   /* nada foi alocado a mais */
    VERIFICA_IGUAL(1, ler_raiz());
    VERIFICA_ARVORE_COM(1);
}

static void teste_busca_com_saida_nula(void) {
    EXIGE(inicializar_arvore() == 0);
    EXIGE(insere(7) == INSERE_OK);
    VERIFICA_IGUAL(0, buscar_valor(7, NULL));   /* existe  */
    VERIFICA_IGUAL(1, buscar_valor(8, NULL));   /* ausente */
}

static void teste_busca_chaves_ausentes(void) {
    EXIGE(inicializar_arvore() == 0);
    for (int i = 1; i <= 100; i++) EXIGE(insere(i * 2) == INSERE_OK);  /* pares 2..200 */

    int falhas = 0;
    for (int i = 1; i <= 100; i++) {
        if (!busca_ok(i * 2)) falhas++;              /* pares existem   */
        if (!busca_ausente(i * 2 - 1)) falhas++;     /* impares nao     */
    }
    VERIFICA_IGUAL(0, falhas);
    VERIFICA(busca_ausente(0));       /* antes da menor chave  */
    VERIFICA(busca_ausente(-1));
    VERIFICA(busca_ausente(202));     /* depois da maior chave */
    VERIFICA(busca_ausente(INT32_MAX));
    VERIFICA(busca_ausente(INT32_MIN));
}

/* ======================================================================
 * 3) inserir_valor: caminhos basicos
 * ====================================================================== */

static void teste_folha_sem_divisao(void) {
    EXIGE(inicializar_arvore() == 0);
    VERIFICA_IGUAL(INSERE_OK, insere(20));
    VERIFICA_IGUAL(INSERE_OK, insere(10));
    VERIFICA_IGUAL(INSERE_OK, insere(30));

    /* ate MAX_CHAVES chaves a raiz continua sendo a mesma folha */
    VERIFICA_IGUAL(1, ler_raiz());
    VERIFICA_IGUAL(2, total_paginas_alocadas());

    No folha;
    EXIGE(ler_no(1, &folha) == 0);
    VERIFICA_IGUAL(NO_FOLHA, folha.tipo);
    VERIFICA_IGUAL(3, folha.numero_chaves);
    VERIFICA_IGUAL(10, folha.registros[0].id);   /* inserido ordenado */
    VERIFICA_IGUAL(20, folha.registros[1].id);
    VERIFICA_IGUAL(30, folha.registros[2].id);
    VERIFICA_ARVORE_COM(3);
}

static void teste_divisao_da_folha_raiz(void) {
    EXIGE(inicializar_arvore() == 0);
    VERIFICA_IGUAL(INSERE_OK, insere(10));
    VERIFICA_IGUAL(INSERE_OK, insere(20));
    VERIFICA_IGUAL(INSERE_OK, insere(30));
    VERIFICA_IGUAL(INSERE_OK, insere(40));   /* 4a chave: divide e cria nova raiz */

    /* meta, folha esquerdauerda, folha direita, nova raiz */
    VERIFICA_IGUAL(4, total_paginas_alocadas());
    int raiz = ler_raiz();
    VERIFICA(raiz != 1);

    No r, esquerda, direita;
    EXIGE(ler_no(raiz, &r) == 0);
    VERIFICA_IGUAL(NO_INTERNO, r.tipo);
    VERIFICA_IGUAL(1, r.numero_chaves);
    VERIFICA_IGUAL(30, r.chaves[0]);          /* 1a chave da folha direita */
    VERIFICA_IGUAL(1, r.filhos[0]);           /* a folha original fica a esquerdauerda */

    EXIGE(ler_no(r.filhos[0], &esquerda) == 0);
    EXIGE(ler_no(r.filhos[1], &direita) == 0);
    VERIFICA_IGUAL(2, esquerda.numero_chaves);
    VERIFICA_IGUAL(10, esquerda.registros[0].id);
    VERIFICA_IGUAL(20, esquerda.registros[1].id);
    VERIFICA_IGUAL(2, direita.numero_chaves);
    VERIFICA_IGUAL(30, direita.registros[0].id);
    VERIFICA_IGUAL(40, direita.registros[1].id);

    VERIFICA_IGUAL(r.filhos[1], esquerda.proxima);   /* encadeamento das folhas */
    VERIFICA_IGUAL(SEM_PAGINA, direita.proxima);

    VERIFICA_ARVORE_COM(4);
    VERIFICA_IGUAL(2, aud.altura);
}

static void teste_chave_duplicada(void) {
    EXIGE(inicializar_arvore() == 0);
    Registro a = { 5, 100 };
    Registro b = { 5, 999 };
    VERIFICA_IGUAL(INSERE_OK, inserir_valor(&a));
    int paginas = total_paginas_alocadas();
    VERIFICA_IGUAL(INSERE_DUPLICADO, inserir_valor(&b));
    VERIFICA_IGUAL(paginas, total_paginas_alocadas());

    Registro lido;
    EXIGE(buscar_valor(5, &lido) == 0);
    VERIFICA_IGUAL(100, lido.valor);     /* valor original preservado */
    VERIFICA_ARVORE_COM(1);
}

static void teste_duplicadas_em_arvore_grande(void) {
    EXIGE(inicializar_arvore() == 0);
    for (int i = 1; i <= 80; i++) EXIGE(insere(i) == INSERE_OK);
    int paginas = total_paginas_alocadas();
    int raiz = ler_raiz();

    int nao_duplicadas = 0;
    for (int i = 1; i <= 80; i++) {
        if (insere(i) != INSERE_DUPLICADO) nao_duplicadas++;
    }
    VERIFICA_IGUAL(0, nao_duplicadas);
    VERIFICA_IGUAL(paginas, total_paginas_alocadas());   /* nenhuma pagina alocada */
    VERIFICA_IGUAL(raiz, ler_raiz());
    VERIFICA_ARVORE_COM(80);
}

/* ======================================================================
 * 4) Ordens de insercao
 * ====================================================================== */

static void teste_insercao_crescente(void) {
    EXIGE(inicializar_arvore() == 0);
    int falhas = 0;
    for (int i = 1; i <= N_GRANDE; i++) if (insere(i) != INSERE_OK) falhas++;
    VERIFICA_IGUAL(0, falhas);

    VERIFICA_ARVORE_COM(N_GRANDE);
    VERIFICA(aud.altura >= 3);
    VERIFICA(total_paginas_alocadas() > NUM_FRAMES);   /* forca expulsoes no cache */

    int nao_achadas = 0;
    for (int i = 1; i <= N_GRANDE; i++) if (!busca_ok(i)) nao_achadas++;
    VERIFICA_IGUAL(0, nao_achadas);
    VERIFICA(busca_ausente(0));
    VERIFICA(busca_ausente(N_GRANDE + 1));
}

static void teste_insercao_decrescente(void) {
    EXIGE(inicializar_arvore() == 0);
    int falhas = 0;
    for (int i = N_GRANDE; i >= 1; i--) if (insere(i) != INSERE_OK) falhas++;
    VERIFICA_IGUAL(0, falhas);

    VERIFICA_ARVORE_COM(N_GRANDE);
    VERIFICA(aud.altura >= 3);

    int nao_achadas = 0;
    for (int i = 1; i <= N_GRANDE; i++) if (!busca_ok(i)) nao_achadas++;
    VERIFICA_IGUAL(0, nao_achadas);
    VERIFICA(busca_ausente(0));
    VERIFICA(busca_ausente(N_GRANDE + 1));
}

static void teste_insercao_alternada_extremos(void) {
    /* 1, N, 2, N-1, 3, ... : sempre nas duas pontas */
    EXIGE(inicializar_arvore() == 0);
    int lo = 1, hi = N_GRANDE, n = 0;
    while (lo <= hi) {
        chaves[n++] = lo++;
        if (lo <= hi) chaves[n++] = hi--;
    }
    int falhas = 0;
    for (int i = 0; i < n; i++) if (insere(chaves[i]) != INSERE_OK) falhas++;
    VERIFICA_IGUAL(0, falhas);
    VERIFICA_IGUAL(N_GRANDE, n);

    VERIFICA_ARVORE_COM(N_GRANDE);
    VERIFICA_IGUAL(0, conta_nao_encontradas(chaves, n));
}

static void teste_insercao_embaralhada(void) {
    EXIGE(inicializar_arvore() == 0);
    for (int i = 0; i < N_GRANDE; i++) chaves[i] = (i + 1) * 7;   /* 7, 14, 21, ... */
    embaralha(chaves, N_GRANDE);

    int falhas = 0;
    for (int i = 0; i < N_GRANDE; i++) if (insere(chaves[i]) != INSERE_OK) falhas++;
    VERIFICA_IGUAL(0, falhas);

    VERIFICA_ARVORE_COM(N_GRANDE);
    VERIFICA_IGUAL(0, conta_nao_encontradas(chaves, N_GRANDE));

    /* nenhum multiplo "nao inserido" pode ser encontrado */
    int falsos_positivos = 0;
    for (int i = 1; i <= 50; i++) if (!busca_ausente(i * 7 + 1)) falsos_positivos++;
    VERIFICA_IGUAL(0, falsos_positivos);
}

static int quantidade_permutacoes, numero_permutacoes;
static int permutacao_atual[6], permutacao_usada[6];
static int permutacoes[720][6];

static void permutacao_recursiva(int k) {
    if (k == numero_permutacoes) {
        memcpy(permutacoes[quantidade_permutacoes++], permutacao_atual, sizeof(permutacao_atual));
        return;
    }
    for (int i = 0; i < numero_permutacoes; i++) {
        if (!permutacao_usada[i]) {
            permutacao_usada[i] = 1;
            permutacao_atual[k] = i;
            permutacao_recursiva(k + 1);
            permutacao_usada[i] = 0;
        }
    }
}

static void gera_permutacoes(int n) {
    quantidade_permutacoes = 0; numero_permutacoes = n;
    memset(permutacao_usada, 0, sizeof(permutacao_usada));
    permutacao_recursiva(0);
}

static void teste_todas_permutacoes_de_4_chaves(void) {
    /* Qualquer ordem de insercao de {10,20,30,40} deve dar a mesma arvore. */
    gera_permutacoes(4);
    VERIFICA_IGUAL(24, quantidade_permutacoes);

    int estruturas_erradas = 0;
    for (int p = 0; p < quantidade_permutacoes; p++) {
        EXIGE(prepara() == 0);
        for (int i = 0; i < 4; i++) {
            if (insere((permutacoes[p][i] + 1) * 10) != INSERE_OK) estruturas_erradas++;
        }

        No r, e, d;
        int ok = 1;
        if (ler_no(ler_raiz(), &r) != 0) ok = 0;
        else if (r.tipo != NO_INTERNO || r.numero_chaves != 1 || r.chaves[0] != 30) ok = 0;
        else if (ler_no(r.filhos[0], &e) != 0 || ler_no(r.filhos[1], &d) != 0) ok = 0;
        else if (e.numero_chaves != 2 || e.registros[0].id != 10 || e.registros[1].id != 20) ok = 0;
        else if (d.numero_chaves != 2 || d.registros[0].id != 30 || d.registros[1].id != 40) ok = 0;
        else if (e.proxima != r.filhos[1] || d.proxima != SEM_PAGINA) ok = 0;
        if (!ok) estruturas_erradas++;
    }
    VERIFICA_IGUAL(0, estruturas_erradas);
}

static void teste_todas_permutacoes_de_6_chaves(void) {
    /* 720 ordens diferentes: invariantes + busca em todas. */
    gera_permutacoes(6);
    VERIFICA_IGUAL(720, quantidade_permutacoes);

    int arvores_invalidas = 0, buscas_falhas = 0;
    for (int p = 0; p < quantidade_permutacoes; p++) {
        EXIGE(prepara() == 0);
        for (int i = 0; i < 6; i++) {
            if (insere((permutacoes[p][i] + 1) * 10) != INSERE_OK) buscas_falhas++;
        }
        if (!audita_arvore() || aud.total_chaves != 6) {
            if (arvores_invalidas == 0)
                printf("    -> permutacao %d: %s\n", p, aud.ok ? "total de chaves errado" : aud.erro);
            arvores_invalidas++;
        }
        for (int i = 1; i <= 6; i++) if (!busca_ok(i * 10)) buscas_falhas++;
    }
    VERIFICA_IGUAL(0, arvores_invalidas);
    VERIFICA_IGUAL(0, buscas_falhas);
}

/* ======================================================================
 * 5) Crescimento da arvore / divisao de nos internos
 * ====================================================================== */

static void teste_raiz_so_muda_quando_altura_cresce(void) {
    EXIGE(inicializar_arvore() == 0);
    for (int i = 0; i < N_GRANDE; i++) chaves[i] = i + 1;
    embaralha(chaves, N_GRANDE);

    int raiz_anterior = ler_raiz(), altura_anterior = altura_arvore();
    int violacoes = 0, mudancas_de_raiz = 0;
    for (int i = 0; i < N_GRANDE; i++) {
        EXIGE(insere(chaves[i]) == INSERE_OK);
        int raiz = ler_raiz(), altura = altura_arvore();
        if (raiz != raiz_anterior) {
            mudancas_de_raiz++;
            if (altura != altura_anterior + 1) violacoes++;   /* nova raiz => altura +1 */
        } else if (altura != altura_anterior) {
            violacoes++;                           /* mesma raiz => mesma altura */
        }
        raiz_anterior = raiz; altura_anterior = altura;
    }
    VERIFICA_IGUAL(0, violacoes);
    VERIFICA(mudancas_de_raiz >= 2);
    VERIFICA_ARVORE_COM(N_GRANDE);
}

static void teste_divisao_de_no_interno(void) {
    EXIGE(inicializar_arvore() == 0);

    /* sobe ate a altura 3: isso so ocorre quando um no interno (a raiz) divide */
    int i = 0;
    while (altura_arvore() < 3 && i < 2000) EXIGE(insere(++i) == INSERE_OK);
    VERIFICA_IGUAL(3, altura_arvore());
    VERIFICA_ARVORE_COM(i);

    No raiz, esquerda, direita;
    EXIGE(ler_no(ler_raiz(), &raiz) == 0);
    VERIFICA_IGUAL(NO_INTERNO, raiz.tipo);
    VERIFICA_IGUAL(1, raiz.numero_chaves);          /* nova raiz: 1 separador promovido */
    EXIGE(ler_no(raiz.filhos[0], &esquerda) == 0);
    EXIGE(ler_no(raiz.filhos[1], &direita) == 0);
    VERIFICA_IGUAL(NO_INTERNO, esquerda.tipo);
    VERIFICA_IGUAL(NO_INTERNO, direita.tipo);
    VERIFICA_IGUAL(2, esquerda.numero_chaves);           /* dividir_interno: 4 chaves -> 2 | promove | 1 */
    VERIFICA_IGUAL(1, direita.numero_chaves);
    VERIFICA_IGUAL(SEM_PAGINA, esquerda.proxima);     /* nos internos nao tem irmao encadeado */
    VERIFICA_IGUAL(SEM_PAGINA, direita.proxima);

    /* continua ate a altura 4 */
    while (altura_arvore() < 4 && i < 5000) EXIGE(insere(++i) == INSERE_OK);
    VERIFICA_IGUAL(4, altura_arvore());
    VERIFICA_ARVORE_COM(i);

    int nao_achadas = 0;
    for (int k = 1; k <= i; k++) if (!busca_ok(k)) nao_achadas++;
    VERIFICA_IGUAL(0, nao_achadas);
}

static void teste_varredura_encadeada_ordenada(void) {
    EXIGE(inicializar_arvore() == 0);
    for (int i = 0; i < 250; i++) chaves[i] = (int32_t)((i + 1) * 13) - 1600;  /* inclui negativas */
    embaralha(chaves, 250);
    for (int i = 0; i < 250; i++) EXIGE(insere(chaves[i]) == INSERE_OK);

    int n = coleta_chaves(coletadas, 4096);
    VERIFICA_IGUAL(250, n);

    qsort(chaves, 250, sizeof(int32_t), cmp_int32);
    int diferencas = 0;
    for (int i = 0; i < n && i < 250; i++) if (coletadas[i] != chaves[i]) diferencas++;
    VERIFICA_IGUAL(0, diferencas);          /* travessia das folhas == ordem total */
    VERIFICA_ARVORE_COM(250);
}

/* ======================================================================
 * 6) Chaves negativas e extremos
 * ====================================================================== */

static void teste_chaves_negativas_e_extremos(void) {
    EXIGE(inicializar_arvore() == 0);
    int32_t especiais[] = { INT32_MIN, INT32_MIN + 1, -1000000, -1000, -2, -1,
                            0, 1, 2, 1000, 1000000, INT32_MAX - 1, INT32_MAX };
    int n = (int)(sizeof(especiais) / sizeof(especiais[0]));
    memcpy(chaves, especiais, sizeof(especiais));
    embaralha(chaves, n);

    int falhas = 0;
    for (int i = 0; i < n; i++) if (insere(chaves[i]) != INSERE_OK) falhas++;
    VERIFICA_IGUAL(0, falhas);

    VERIFICA_ARVORE_COM(n);
    VERIFICA_IGUAL(0, conta_nao_encontradas(especiais, n));
    VERIFICA(busca_ausente(-3));
    VERIFICA(busca_ausente(3));
    VERIFICA_IGUAL(INSERE_DUPLICADO, insere(INT32_MIN));
    VERIFICA_IGUAL(INSERE_DUPLICADO, insere(INT32_MAX));

    /* ordem de varredura respeita a ordem com sinal */
    int total = coleta_chaves(coletadas, 4096);
    VERIFICA_IGUAL(n, total);
    int fora_de_ordem = 0;
    for (int i = 1; i < total; i++) if (coletadas[i] <= coletadas[i - 1]) fora_de_ordem++;
    VERIFICA_IGUAL(0, fora_de_ordem);
    VERIFICA_IGUAL(INT32_MIN, coletadas[0]);
    VERIFICA_IGUAL(INT32_MAX, coletadas[total - 1]);
}

static void teste_valores_extremos_no_registro(void) {
    EXIGE(inicializar_arvore() == 0);
    Registro a = { 1, INT32_MIN };
    Registro b = { 2, INT32_MAX };
    Registro c = { 3, -1 };
    Registro d = { 4, 0 };
    VERIFICA_IGUAL(INSERE_OK, inserir_valor(&a));
    VERIFICA_IGUAL(INSERE_OK, inserir_valor(&b));
    VERIFICA_IGUAL(INSERE_OK, inserir_valor(&c));
    VERIFICA_IGUAL(INSERE_OK, inserir_valor(&d));   /* forca divisao com esses valores */

    Registro r;
    EXIGE(buscar_valor(1, &r) == 0); VERIFICA_IGUAL(INT32_MIN, r.valor);
    EXIGE(buscar_valor(2, &r) == 0); VERIFICA_IGUAL(INT32_MAX, r.valor);
    EXIGE(buscar_valor(3, &r) == 0); VERIFICA_IGUAL(-1, r.valor);
    EXIGE(buscar_valor(4, &r) == 0); VERIFICA_IGUAL(0, r.valor);
}

/* ======================================================================
 * 7) Testes diretos (caixa-branca) das divisoes e da serializacao
 * ====================================================================== */

static void teste_dividir_folha_direto(void) {
    EXIGE(inicializar_arvore() == 0);

    No no;
    memset(&no, 0, sizeof(no));
    no.tipo = NO_FOLHA;
    no.numero_chaves = 4;                   /* folha "estourada": MAX_CHAVES + 1 */
    no.proxima = 77;                     /* irma fictícia que deve ir para a direita */
    for (int i = 0; i < 4; i++) {
        no.registros[i].id = (i + 1) * 10;
        no.registros[i].valor = (i + 1) * 100;
    }

    int32_t promovida = 0;
    int nova = -1;
    VERIFICA_IGUAL(INSERE_DIVIDIU, dividir_folha(1, &no, &promovida, &nova));

    VERIFICA_IGUAL(30, promovida);                       /* coperacaoia da 1a chave da direita */
    VERIFICA_IGUAL(2, nova);                             /* proxima pagina livre */
    VERIFICA_IGUAL(3, total_paginas_alocadas());

    No esquerda, direita;
    EXIGE(ler_no(1, &esquerda) == 0);
    EXIGE(ler_no(nova, &direita) == 0);
    VERIFICA_IGUAL(NO_FOLHA, esquerda.tipo);
    VERIFICA_IGUAL(NO_FOLHA, direita.tipo);
    VERIFICA_IGUAL(2, esquerda.numero_chaves);
    VERIFICA_IGUAL(2, direita.numero_chaves);
    VERIFICA_IGUAL(10, esquerda.registros[0].id);
    VERIFICA_IGUAL(20, esquerda.registros[1].id);
    VERIFICA_IGUAL(30, direita.registros[0].id);
    VERIFICA_IGUAL(40, direita.registros[1].id);
    VERIFICA_IGUAL(300, direita.registros[0].valor);         /* valores acompanham os ids */
    VERIFICA_IGUAL(400, direita.registros[1].valor);
    VERIFICA_IGUAL(nova, esquerda.proxima);                   /* esquerda -> direita */
    VERIFICA_IGUAL(77, direita.proxima);                     /* direita herda o antigo proxima */
}

static void teste_dividir_interno_direto(void) {
    EXIGE(inicializar_arvore() == 0);

    No no;
    memset(&no, 0, sizeof(no));
    no.tipo = NO_INTERNO;
    no.numero_chaves = 4;                   /* interno "estourado": 4 chaves, 5 filhos */
    no.proxima = SEM_PAGINA;
    for (int i = 0; i < 4; i++) no.chaves[i] = (i + 1) * 10;
    for (int i = 0; i < 5; i++) no.filhos[i] = 100 + i;

    int32_t promovida = 0;
    int nova = -1;
    VERIFICA_IGUAL(INSERE_DIVIDIU, dividir_interno(1, &no, &promovida, &nova));

    VERIFICA_IGUAL(30, promovida);       /* chave do meio SOBE (nao fica em nenhum filho) */
    VERIFICA_IGUAL(2, nova);

    No esquerda, direita;
    EXIGE(ler_no(1, &esquerda) == 0);
    EXIGE(ler_no(nova, &direita) == 0);

    VERIFICA_IGUAL(NO_INTERNO, esquerda.tipo);
    VERIFICA_IGUAL(2, esquerda.numero_chaves);
    VERIFICA_IGUAL(10, esquerda.chaves[0]);
    VERIFICA_IGUAL(20, esquerda.chaves[1]);
    VERIFICA_IGUAL(100, esquerda.filhos[0]);
    VERIFICA_IGUAL(101, esquerda.filhos[1]);
    VERIFICA_IGUAL(102, esquerda.filhos[2]);

    VERIFICA_IGUAL(NO_INTERNO, direita.tipo);
    VERIFICA_IGUAL(1, direita.numero_chaves);
    VERIFICA_IGUAL(40, direita.chaves[0]);
    VERIFICA_IGUAL(103, direita.filhos[0]);
    VERIFICA_IGUAL(104, direita.filhos[1]);
    VERIFICA_IGUAL(SEM_PAGINA, direita.proxima);
}

static void teste_serializacao_de_no(void) {
    unsigned char pagina[TAMANHO_PAGINA];
    No a, b;

    /* ---- folha ---- */
    memset(&a, 0, sizeof(a));
    a.tipo = NO_FOLHA;
    a.numero_chaves = 3;
    a.proxima = SEM_PAGINA;
    a.registros[0].id = -5;       a.registros[0].valor = 50;
    a.registros[1].id = 0;        a.registros[1].valor = 0;
    a.registros[2].id = INT32_MAX; a.registros[2].valor = INT32_MIN;

    serializar_no(&a, pagina);
    VERIFICA_IGUAL(NO_FOLHA, pagina[0]);                 /* little-endian */
    VERIFICA_IGUAL(3, pagina[4]);
    VERIFICA_IGUAL(0xFF, pagina[8]);                     /* proxima = -1 */
    VERIFICA_IGUAL(0xFF, pagina[11]);
    VERIFICA_IGUAL(0xFB, pagina[TAMANHO_CABECALHO]);     /* id -5 = FB FF FF FF */
    VERIFICA_IGUAL(0xFF, pagina[TAMANHO_CABECALHO + 3]);

    int resto_zerado = 1;
    for (int i = TAMANHO_CABECALHO + 3 * TAMANHO_REGISTRO; i < TAMANHO_PAGINA; i++)
        if (pagina[i] != 0) { resto_zerado = 0; break; }
    VERIFICA(resto_zerado);                              /* sem lixo no restante da pagina */

    desserializar_no(pagina, &b);
    VERIFICA_IGUAL(NO_FOLHA, b.tipo);
    VERIFICA_IGUAL(3, b.numero_chaves);
    VERIFICA_IGUAL(SEM_PAGINA, b.proxima);
    for (int i = 0; i < 3; i++) {
        VERIFICA_IGUAL(a.registros[i].id, b.registros[i].id);
        VERIFICA_IGUAL(a.registros[i].valor, b.registros[i].valor);
    }

    /* ---- interno ---- */
    memset(&a, 0, sizeof(a));
    a.tipo = NO_INTERNO;
    a.numero_chaves = 3;
    a.proxima = SEM_PAGINA;
    a.chaves[0] = 10; a.chaves[1] = -20; a.chaves[2] = 30;
    a.filhos[0] = 1; a.filhos[1] = 2; a.filhos[2] = 3; a.filhos[3] = 4;

    serializar_no(&a, pagina);
    VERIFICA_IGUAL(10, pagina[OFFSET_CHAVES]);
    VERIFICA_IGUAL(1,  pagina[OFFSET_FILHOS]);
    VERIFICA_IGUAL(4,  pagina[OFFSET_FILHOS + 3 * 4]);   /* ultimo filho */

    desserializar_no(pagina, &b);
    VERIFICA_IGUAL(NO_INTERNO, b.tipo);
    VERIFICA_IGUAL(3, b.numero_chaves);
    for (int i = 0; i < 3; i++) VERIFICA_IGUAL(a.chaves[i], b.chaves[i]);
    for (int i = 0; i < 4; i++) VERIFICA_IGUAL(a.filhos[i], b.filhos[i]);

    /* ---- numero_chaves corrompido e tratado como no vazio ---- */
    serializar_no(&a, pagina);
    serializa_uint32_le(99, pagina + 4);
    desserializar_no(pagina, &b);
    VERIFICA_IGUAL(0, b.numero_chaves);
    serializa_uint32_le((uint32_t)-1, pagina + 4);
    desserializar_no(pagina, &b);
    VERIFICA_IGUAL(0, b.numero_chaves);
}

/* ======================================================================
 * 8) Persistencia, cache e fixacoes
 * ====================================================================== */

static void teste_persistencia_apos_reinicio(void) {
    EXIGE(inicializar_arvore() == 0);
    int n = 150;
    for (int i = 0; i < n; i++) chaves[i] = i * 3 - 60;     /* inclui negativas */
    embaralha(chaves, n);
    for (int i = 0; i < n; i++) EXIGE(insere(chaves[i]) == INSERE_OK);

    int raiz_anteriores = ler_raiz();
    int paginas_antes = total_paginas_alocadas();
    int altura_antes = altura_arvore();

    /* "fecha" o programa: grava o que estava sujo e zera o cache */
    EXIGE(reinicia_cache_mantendo_disco() == 0);
    long leituras_antes = contador_leituras_disco;

    VERIFICA_IGUAL(0, inicializar_arvore());
    VERIFICA_IGUAL(paginas_antes, total_paginas_alocadas());   /* nao recria nada */
    VERIFICA_IGUAL(raiz_anteriores, ler_raiz());
    VERIFICA_IGUAL(altura_antes, altura_arvore());

    VERIFICA_IGUAL(0, conta_nao_encontradas(chaves, n));
    VERIFICA(contador_leituras_disco > leituras_antes);        /* leu do disco de fato */
    VERIFICA_ARVORE_COM(n);

    /* a arvore reaberta continua aceitando insercoes e duplicatas */
    VERIFICA_IGUAL(INSERE_DUPLICADO, insere(chaves[0]));
    int novas = 0;
    for (int i = 0; i < 100; i++) {
        int32_t id = 1000 + i;
        if (insere(id) != INSERE_OK) novas++;
    }
    VERIFICA_IGUAL(0, novas);
    VERIFICA_ARVORE_COM(n + 100);
    VERIFICA_IGUAL(0, conta_nao_encontradas(chaves, n));
    VERIFICA(busca_ok(1000));
    VERIFICA(busca_ok(1099));
}

static void teste_persistencia_com_varias_reaberturas(void) {
    EXIGE(inicializar_arvore() == 0);
    int total = 0;
    for (int ciclo = 0; ciclo < 5; ciclo++) {
        for (int i = 0; i < 40; i++) {
            EXIGE(insere(ciclo * 1000 + i) == INSERE_OK);
            total++;
        }
        EXIGE(reinicia_cache_mantendo_disco() == 0);
        EXIGE(inicializar_arvore() == 0);
        VERIFICA_ARVORE_COM(total);
    }
    int nao_achadas = 0;
    for (int ciclo = 0; ciclo < 5; ciclo++)
        for (int i = 0; i < 40; i++)
            if (!busca_ok(ciclo * 1000 + i)) nao_achadas++;
    VERIFICA_IGUAL(0, nao_achadas);
}

static void teste_nenhuma_pagina_fica_fixada(void) {
    EXIGE(inicializar_arvore() == 0);
    for (int i = 1; i <= 120; i++) EXIGE(insere(i) == INSERE_OK);
    VERIFICA_IGUAL(0, fixacoes_pendentes());

    for (int i = 1; i <= 120; i++) (void)busca_ok(i);
    (void)busca_ausente(9999);
    VERIFICA_IGUAL(0, fixacoes_pendentes());

    for (int i = 1; i <= 120; i++) (void)insere(i);            /* duplicatas */
    VERIFICA_IGUAL(0, fixacoes_pendentes());
}

/* ======================================================================
 * 9) Teste de estresse comparado a um modelo simples
 * ====================================================================== */

#define FAIXA_ESTRESSE 401         /* ids em [-200, 200]: muitas colisoes */

static void teste_estresse_contra_modelo(void) {
    static char presente[FAIXA_ESTRESSE];
    memset(presente, 0, sizeof(presente));
    EXIGE(inicializar_arvore() == 0);

    long desvios = 0;
    int distintas = 0;
    for (int operacao = 0; operacao < 1500; operacao++) {
        int32_t id = (int32_t)(proximo_aleatorio() % FAIXA_ESTRESSE) - 200;
        int indice = id + 200;
        int tipo_operacao = (int)((proximo_aleatorio() >> 8) % 4);

        if (tipo_operacao < 3) {                              /* insercao */
            int esperado = presente[indice] ? INSERE_DUPLICADO : INSERE_OK;
            int obtido = insere(id);
            if (obtido != esperado) {
                if (desvios++ == 0)
                    printf("    -> operacao %d: insere(%d) retornou %d, esperado %d\n",
                           operacao, id, obtido, esperado);
            }
            if (!presente[indice]) { presente[indice] = 1; distintas++; }
        } else {                                        /* busca */
            int ok = presente[indice] ? busca_ok(id) : busca_ausente(id);
            if (!ok) {
                if (desvios++ == 0)
                    printf("    -> operacao %d: busca(%d) divergiu do modelo (presente=%d)\n",
                           operacao, id, presente[indice]);
            }
        }
    }
    VERIFICA_IGUAL(0, desvios);

    VERIFICA_ARVORE_COM(distintas);
    int n = coleta_chaves(coletadas, 4096);
    VERIFICA_IGUAL(distintas, n);

    int divergencias_finais = 0;
    for (int i = 0; i < FAIXA_ESTRESSE; i++) {
        int32_t id = i - 200;
        if (presente[i] ? !busca_ok(id) : !busca_ausente(id)) divergencias_finais++;
    }
    VERIFICA_IGUAL(0, divergencias_finais);
}

/* ======================================================================
 * Execucao
 * ====================================================================== */

#define TEMPO_LIMITE_TESTE_S 60

/* Cada teste roda em um processo filho: se o codigo testado travar, entrar em
 * recursao infinita ou der segfault, isso vira uma falha do teste e a bateria
 * continua com os demais. */
static void executa_teste(const char *nome, void (*fn)(void)) {
    static long *verif_compartilhada = NULL;
    if (verif_compartilhada == NULL) {
        verif_compartilhada = mmap(NULL, sizeof(long), PROT_READ | PROT_WRITE,
                                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (verif_compartilhada == MAP_FAILED) { perror("mmap"); exit(2); }
    }
    *verif_compartilhada = 0;

    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); exit(2); }

    if (pid == 0) {                                   /* ---- filho ---- */
        alarm(TEMPO_LIMITE_TESTE_S);
        verificacoes = 0;
        falhas_teste_atual = 0;
        rng_estado = SEMENTE;
        reinicia_ambiente();

        fn();

        /* regra global: nenhum teste pode terminar com pagina fixada no cache */
        verificacoes++;
        if (fixacoes_pendentes() != 0) {
            falhas_teste_atual++;
            printf("    -> %d fixacao(oes) pendente(s) ao final do teste (fixa/solta desbalanceados)\n",
                   fixacoes_pendentes());
        }
        *verif_compartilhada = verificacoes;
        fflush(stdout);
        _exit(falhas_teste_atual == 0 ? 0 : 1);
    }

    int status = 0;                                   /* ---- pai ---- */
    waitpid(pid, &status, 0);
    verificacoes += *verif_compartilhada;
    testes_executados++;

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        printf("[ OK ] %s\n", nome);
    } else if (WIFSIGNALED(status)) {
        testes_falhos++;
        int sig = WTERMSIG(status);
        printf("[FALHA] %s (processo encerrado pelo sinal %d: %s)\n",
               nome, sig, sig == SIGALRM ? "tempo limite excedido" : strsignal(sig));
    } else {
        testes_falhos++;
        printf("[FALHA] %s\n", nome);
    }
}

#define EXECUTA(fn) executa_teste(#fn, fn)

int main(void) {
    char modelo[] = "/tmp/teste_bmais_XXXXXX";
    char cwd[4096];
    if (getcwd(cwd, sizeof(cwd)) == NULL) { perror("getcwd"); return 2; }
    if (mkdtemp(modelo) == NULL)          { perror("mkdtemp"); return 2; }
    if (chdir(modelo) != 0)               { perror("chdir"); return 2; }

    printf("Testes da Arvore B+ (MAX_CHAVES=%d, NUM_FRAMES=%d, diretorio: %s)\n\n",
           MAX_CHAVES, NUM_FRAMES, modelo);

    printf("-- inicializacao --\n");
    EXECUTA(teste_inicializa_banco_vazio);
    EXECUTA(teste_inicializa_e_idempotente);
    EXECUTA(teste_banco_nao_inicializado);

    printf("\n-- busca --\n");
    EXECUTA(teste_busca_em_arvore_vazia);
    EXECUTA(teste_insere_e_busca_uma_chave);
    EXECUTA(teste_busca_com_saida_nula);
    EXECUTA(teste_busca_chaves_ausentes);

    printf("\n-- insercao basica --\n");
    EXECUTA(teste_folha_sem_divisao);
    EXECUTA(teste_divisao_da_folha_raiz);
    EXECUTA(teste_chave_duplicada);
    EXECUTA(teste_duplicadas_em_arvore_grande);

    printf("\n-- ordens de insercao --\n");
    EXECUTA(teste_insercao_crescente);
    EXECUTA(teste_insercao_decrescente);
    EXECUTA(teste_insercao_alternada_extremos);
    EXECUTA(teste_insercao_embaralhada);
    EXECUTA(teste_todas_permutacoes_de_4_chaves);
    EXECUTA(teste_todas_permutacoes_de_6_chaves);

    printf("\n-- crescimento e divisao de nos --\n");
    EXECUTA(teste_raiz_so_muda_quando_altura_cresce);
    EXECUTA(teste_divisao_de_no_interno);
    EXECUTA(teste_varredura_encadeada_ordenada);

    printf("\n-- valores extremos --\n");
    EXECUTA(teste_chaves_negativas_e_extremos);
    EXECUTA(teste_valores_extremos_no_registro);

    printf("\n-- caixa-branca --\n");
    EXECUTA(teste_dividir_folha_direto);
    EXECUTA(teste_dividir_interno_direto);
    EXECUTA(teste_serializacao_de_no);

    printf("\n-- persistencia e cache --\n");
    EXECUTA(teste_persistencia_apos_reinicio);
    EXECUTA(teste_persistencia_com_varias_reaberturas);
    EXECUTA(teste_nenhuma_pagina_fica_fixada);

    printf("\n-- estresse --\n");
    EXECUTA(teste_estresse_contra_modelo);

    /* limpeza do diretorio temporario */
    unlink(ARQUIVO_BD);
    if (chdir(cwd) == 0) rmdir(modelo);

    printf("\nResumo: %d teste(s), %d falha(s), %ld verificacao(oes)\n",
           testes_executados, testes_falhos, verificacoes);
    
    return testes_falhos == 0 ? 0 : 1;
}
