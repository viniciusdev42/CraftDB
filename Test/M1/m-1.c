#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> /* fsync */
#include <fcntl.h>  /* fcntl, struct flock - travamento por intervalo de bytes */

#include "m-1.h"

/* ---------------------------------------------------------------------
 * Funcoes auxiliares internas
 * --------------------------------------------------------------------- */

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

/* Retorna o tamanho atual do arquivo do banco, em bytes (0 se nao existir) */
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

/*
 * Trava (ou destrava) exatamente o intervalo de bytes pertencente a
 * pagina n: [n * TAMANHO_PAGINA, n * TAMANHO_PAGINA + TAMANHO_PAGINA - 1].
 * tipo_trava deve ser F_RDLCK (leitura), F_WRLCK (escrita) ou F_UNLCK.
 *
 * Como a trava cobre apenas os bytes da propria pagina, uma escrita na
 * pagina n nunca disputa nem interfere com os bytes de qualquer outra
 * pagina m != n, mesmo havendo acessos concorrentes ao arquivo.
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
 * Escreve TAMANHO_PAGINA bytes de buffer na posicao da pagina n do
 * arquivo, sem checar se a pagina ja estava alocada. Usada internamente
 * por escreve_pagina() e por aloca_pagina().
 *
 * A escrita fica delimitada ao intervalo exato de bytes da pagina n
 * (mesmo offset de inicio, mesmo tamanho fixo de TAMANHO_PAGINA) e e
 * protegida por uma trava (F_WRLCK) restrita a esse mesmo intervalo,
 * garantindo que os bytes de outras paginas nunca sejam tocados.
 */
static int escreve_pagina_bruta(int n, const unsigned char *buffer) {
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

/* ---------------------------------------------------------------------
 * Funcoes publicas
 * --------------------------------------------------------------------- */

void serializa_registro(const Registro *registro, unsigned char *buffer) {
    serializa_uint32_le((uint32_t)registro->id, buffer);
    serializa_uint32_le((uint32_t)registro->valor, buffer + 4);
}

void desserializa_registro(const unsigned char *buffer, Registro *registro) {
    registro->id    = (int32_t)desserializa_uint32_le(buffer);
    registro->valor = (int32_t)desserializa_uint32_le(buffer + 4);
}

int total_paginas_alocadas(void) {
    return (int)(tamanho_arquivo() / TAMANHO_PAGINA);
}

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

int aloca_pagina(void) {
    int nova_pagina = total_paginas_alocadas();
    unsigned char pagina_vazia[TAMANHO_PAGINA];
    memset(pagina_vazia, 0, TAMANHO_PAGINA);

    if (escreve_pagina_bruta(nova_pagina, pagina_vazia) != 0) {
        return -1;
    }
    return nova_pagina;
}

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
    return 0;
}

int escreve_pagina(int n, unsigned char *buffer, size_t tamanho_buffer) {
    if (buffer == NULL || tamanho_buffer != TAMANHO_PAGINA) {
        fprintf(stderr, "Erro: buffer invalido (deve ter exatamente %d bytes)\n", TAMANHO_PAGINA);
        return -1;
    }
    if (n < 0 || n >= total_paginas_alocadas()) {
        fprintf(stderr, "Erro: pagina %d nao foi alocada. Use aloca_pagina() antes de escrever.\n", n);
        return -1;
    }

    return escreve_pagina_bruta(n, buffer);
}
