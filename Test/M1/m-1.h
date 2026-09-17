#ifndef BANCO_PAGINAS_H
#define BANCO_PAGINAS_H

#include <stddef.h> /* size_t  */
#include <stdint.h> /* int32_t */

#define TAMANHO_PAGINA       4096  /* 4KB */
#define TAMANHO_CABECALHO    16    /* 16B */
#define TAMANHO_REGISTRO     8     /* 8B  */
#define REGISTROS_POR_PAGINA ((TAMANHO_PAGINA - TAMANHO_CABECALHO) / TAMANHO_REGISTRO)

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

/* Converte registro para TAMANHO_REGISTRO bytes em little-endian. */
void serializa_registro(const Registro *registro, unsigned char *buffer);

/* Reconstroi um Registro a partir de TAMANHO_REGISTRO bytes em little-endian. */
void desserializa_registro(const unsigned char *buffer, Registro *registro);

/* Quantas paginas ja foram alocadas (existem) no arquivo do banco. */
int total_paginas_alocadas(void);

/* Forca a gravacao fisica em disco de tudo que ja foi escrito.
 * Retorna 0 em sucesso, -1 em erro. */
int sincroniza_dados(void);

/* Aloca uma nova pagina (zerada) no fim do arquivo.
 * Retorna o numero da pagina recem-criada, ou -1 em erro. */
int aloca_pagina(void);

/* Le TAMANHO_PAGINA bytes da pagina n para buffer.
 * Valida buffer (tamanho e NULL) e numero da pagina.
 * Retorna 0 em sucesso, -1 em erro. */
int ler_pagina(int n, unsigned char *buffer, size_t tamanho_buffer);

/* Escreve TAMANHO_PAGINA bytes de buffer na pagina n (ja alocada).
 * Valida buffer (tamanho e NULL) e numero da pagina.
 * Retorna 0 em sucesso, -1 em erro. */
int escreve_pagina(int n, unsigned char *buffer, size_t tamanho_buffer);

#endif /* BANCO_PAGINAS_H */
