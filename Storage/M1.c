#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> // fsync - Gravação em Disco
#include<fcntl.h> // struct flock - Isolamento de Páginas

#define TAMANHO_PAGINA 4096   // 4KB
#define TAMANHO_CABECALHO 16  // 16B   
#define TAMANHO_REGISTRO 8    // 8B 
#define REGISTROS_POR_PAGINA ((TAMANHO_PAGINA - TAMANHO_CABECALHO) / TAMANHO_REGISTRO) // 510B

#define ARQUIVO_BD "craft-db.dat"

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

    return 0;
}
