#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> /* fsync */
#include <fcntl.h>  /* fcntl, struct flock - travamento por intervalo de bytes */
#include <stddef.h> /* size_t  */
#include <stdint.h> /* int32_t */

#define TAMANHO_PAGINA       4096  /* 4KB */
#define TAMANHO_CABECALHO    16    /* 16B */
#define TAMANHO_REGISTRO     8     /* 8B  */
#define REGISTROS_POR_PAGINA ((TAMANHO_PAGINA - TAMANHO_CABECALHO) / TAMANHO_REGISTRO) /* 510B */

#define ARQUIVO_BD "craft-db.dat"

/*
 * Representacao em memoria de um registro (8B): um id e um valor, cada
 * um com 4 bytes. Em disco, os dois campos sao sempre armazenados em
 * little-endian, independente da arquitetura da CPU que estiver
 * rodando o programa (serializa_registro/desserializa_registro cuidam
 * dessa conversao byte a byte).
 */
typedef struct {
    int32_t id;
    int32_t valor;
} Registro;

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

int main(void) {
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

    /* Carrega a página atual (para não sobrescrever o restante do conteúdo) */
    if (ler_pagina(pagina_alvo, buffer, sizeof(buffer)) != 0) {
        return 1;
    }

    /* Monta o registro e o serializa para little-endian antes de gravar */
    Registro registro = { .id = 1, .valor = 42 };
    unsigned char registro_bytes[TAMANHO_REGISTRO];
    serializa_registro(&registro, registro_bytes);

    /* Calcula o deslocamento do slot dentro da página, após o cabeçalho */
    int offset_na_pagina = TAMANHO_CABECALHO + slot_alvo * TAMANHO_REGISTRO;

    /* Copia os bytes serializados para a posição do slot no buffer da página */
    memcpy(buffer + offset_na_pagina, registro_bytes, TAMANHO_REGISTRO);

    /* Grava a página de volta no arquivo (já sincroniza com o disco) */
    if (escreve_pagina(pagina_alvo, buffer, sizeof(buffer)) != 0) {
        return 1;
    }

    /* Sincronização explícita adicional, garantindo persistência total */
    sincroniza_dados();

    /* Calcula o byte absoluto no arquivo onde o registro foi gravado */
    long byte_absoluto = (long)pagina_alvo * TAMANHO_PAGINA + offset_na_pagina;

    printf("Registro (id=%d, valor=%d) gravado no slot %d da pagina %d.\n",
           registro.id, registro.valor, slot_alvo, pagina_alvo);
    printf("O registro está no byte: %ld\n", byte_absoluto);

    /* Relê a página e desserializa o registro, só para demonstrar o round-trip */
    unsigned char pagina_lida[TAMANHO_PAGINA];
    Registro registro_lido;
    ler_pagina(pagina_alvo, pagina_lida, sizeof(pagina_lida));
    desserializa_registro(pagina_lida + offset_na_pagina, &registro_lido);
    printf("Registro relido do disco e desserializado: id=%d, valor=%d\n",
           registro_lido.id, registro_lido.valor);

    return 0;
}
