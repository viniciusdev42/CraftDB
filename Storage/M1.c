#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> // fsync - Gravação em disco

#define TAMANHO_PAGINA 4096   // 4KB
#define TAMANHO_CABECALHO 16  // 16B   
#define TAMANHO_REGISTRO 8    // 8B 
#define REGISTROS_POR_PAGINA ((TAMANHO_PAGINA - TAMANHO_CABECALHO) / TAMANHO_REGISTRO) // 510B

#define ARQUIVO_BD "craft-db.dat"

/*
 Lê 4KB da página n do arquivo e coloca o conteúdo em buffer.
 O buffer deve ter pelo menos 4096 bytes.
 */

 void ler_pagina(int n, unsigned char *buffer) {
    FILE *f = fopen(ARQUIVO_BD, "rb");
    if (f == NULL) {
        memset(buffer, 0, TAMANHO_PAGINA);
        return;
    }
    fseek(f, (long)n * TAMANHO_PAGINA, SEEK_SET);
    fread(buffer, 1, TAMANHO_PAGINA, f);
    fclose(f);
}

/*
  Escreve os 4KB de buffer na página n do arquivo.
  Armazena os dados em disco a fim de manter a persistência
 */

 void escreve_pagina(int n, unsigned char *buffer) {
    FILE *f = fopen(ARQUIVO_BD, "r+b");
    if (f == NULL) {
        f = fopen(ARQUIVO_BD, "w+b");
    }
    fseek(f, (long)n * TAMANHO_PAGINA, SEEK_SET);
    fwrite(buffer, 1, TAMANHO_PAGINA, f);
    fflush(f);
    fsync(fileno(f)); // Envia os dados do buffer para o SO e os grava no disco
    fclose(f);
}

int main(void) {
    int pagina_alvo = 2;
    int slot_alvo = 0;

    unsigned char buffer[TAMANHO_PAGINA];
    unsigned char registro[TAMANHO_REGISTRO] = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08
    };

    // Carrega a página atual 
    ler_pagina(pagina_alvo, buffer);

    // Calcula o deslocamento do slot dentro da página,após o cabeçalho 
    int offset_na_pagina = TAMANHO_CABECALHO + slot_alvo * TAMANHO_REGISTRO;

    // Copia o registro para a posição do slot dentro do buffer da página
    memcpy(buffer + offset_na_pagina, registro, TAMANHO_REGISTRO);

    //  Grava a página de volta no arquivo 
    escreve_pagina(pagina_alvo, buffer);

    // Calcula o byte no arquivo onde o registro foi gravado 
    long byte_absoluto = (long)pagina_alvo * TAMANHO_PAGINA + offset_na_pagina;

    printf("O registro começa no byte: %ld\n", byte_absoluto);

    return 0;
}
