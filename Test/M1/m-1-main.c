#include <stdio.h>
#include <string.h>

#include "m-1.h"

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
