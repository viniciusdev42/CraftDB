#ifndef PAGER
#define PAGER

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L  
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> /* Gravação em Disco*/ 
#include <fcntl.h>  /* Isolamento entre Páginas */
#include <stddef.h> 
#include <stdint.h>

#define TAMANHO_PAGINA  4096  /* 4KB */
#define TAMANHO_CABECALHO 16  /* 16B */
#define TAMANHO_REGISTRO  8   /* 8B */  
#define REGISTROS_POR_PAGINA ((TAMANHO_PAGINA - TAMANHO_CABECALHO) / TAMANHO_REGISTRO) /* 510B */

#define ARQUIVO_CRAFT_DB "craft-db.dat"

#define CAMINHO_ARQUIVO 256

/* Arquivo de dados em uso. Comeca no padrao ARQUIVO_DB e pode ser trocado
   em tempo de execucao por configura_arquivo(). */

static char ARQUIVO_DB[CAMINHO_ARQUIVO] = ARQUIVO_CRAFT_DB;

/*
 configura_arquivo(caminho)
 Define o arquivo de dados que o Pager passa a usar nas proximas operacoes
 (leitura, escrita, alocacao, sincronizacao). O arquivo nao e' aberto nem criado
 aqui: ele sera criado na primeira escrita/alocacao, se nao existir.

 IMPORTANTE: o cache de paginas nao sabe qual arquivo originou cada frame.
 Antes de trocar de arquivo, quem chama deve descarregar/reiniciar o cache
 (reiniciar_cache()), como ja faz selecionar_arquivo_tabela().

 Retorna 0 em caso de sucesso, ou -1 se o caminho for NULL, vazio ou longo demais.
*/

int configura_arquivo(const char *caminho) {
    if (caminho == NULL || caminho[0] == '\0') {
        fprintf(stderr, "Erro: caminho de arquivo invalido\n");
        return -1;
    }
    if (strlen(caminho) >= CAMINHO_ARQUIVO) {
        fprintf(stderr, "Erro: caminho de arquivo longo demais (maximo %d caracteres)\n",
                CAMINHO_ARQUIVO - 1);
        return -1;
    }
    strcpy(ARQUIVO_DB, caminho);
    return 0;
}


/*
   Struct para operações de serialização e desserialização
*/

typedef struct {
    int32_t id;
    int32_t valor;
} Registro;

/* Contador global de leituras físicas de página em disco (incrementado apenas
   dentro de ler_pagina(), em toda leitura bem-sucedida). Serve como uma sonda
   independente das estatísticas do cache, util para testes de aceitação que
   precisam confirmar que uma página foi de fato lida do disco (ou não). 
*/

static long contador_leituras_disco = 0;

/*  Funções internas*/

static int      trava_pagina(FILE *f, int n, short tipo_trava);
static int      escreve_pagina_buffer(int n, const unsigned char *buffer);
static void     serializa_uint32_le(uint32_t valor, unsigned char *destino);
static uint32_t desserializa_uint32_le(const unsigned char *origem);

/* Retorna o tamanho do arquivo em bytes */

static long tamanho_arquivo(void) {
    long tamanho = 0;
    FILE *f = fopen(ARQUIVO_DB, "rb");
    if (f != NULL) {
        fseek(f, 0, SEEK_END);
        tamanho = ftell(f);
        fclose(f);
    }
    return tamanho;
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

int total_paginas_alocadas(void) {
    return (int)(tamanho_arquivo() / TAMANHO_PAGINA);
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

    FILE *f = fopen(ARQUIVO_DB, "rb");
    if (f == NULL) {
        fprintf(stderr, "Erro: nao foi possivel abrir '%s'\n", ARQUIVO_DB);
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
  Garante que os dados mantenham-se persistidos.
  Retorna 0 em caso de sucesso,ou -1 em caso de erro
*/

int sincroniza_dados(void) {
    FILE *f = fopen(ARQUIVO_DB, "r+b");
    if (f == NULL) {
        fprintf(stderr, "Erro: nao foi possivel abrir '%s' para sincronizar\n", ARQUIVO_DB);
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

/* 
  Escreve 4KB de buffer na posicao da pagina n do arquivo.
  A escrita fica delimitada ao intervalo exato de bytes da pagina n
  possuindo o mesmo offset de inicio, e o mesmo tamanho fixo de TAMANHO_PAGINA.
  É protegida por uma trava (F_WRLCK) restrita a esse mesmo intervalo,
  garantindo que os bytes de outras paginas nunca sejam tocados.
  É usada por escreve_pagina() e aloca_pagina()
*/

static int escreve_pagina_buffer(int n, const unsigned char *buffer) {
    FILE *f = fopen(ARQUIVO_DB, "r+b");
    if (f == NULL) {
        f = fopen(ARQUIVO_DB, "w+b");
        if (f == NULL) {
            fprintf(stderr, "Erro: nao foi possivel abrir/criar '%s'\n", ARQUIVO_DB);
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

void serializa_registro(const Registro *registro, unsigned char *buffer) {
    serializa_uint32_le((uint32_t)registro->id, buffer);
    serializa_uint32_le((uint32_t)registro->valor, buffer + 4);
}

void desserializa_registro(const unsigned char *buffer, Registro *registro) {
    registro->id    = (int32_t)desserializa_uint32_le(buffer);
    registro->valor = (int32_t)desserializa_uint32_le(buffer + 4);
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

#endif 
