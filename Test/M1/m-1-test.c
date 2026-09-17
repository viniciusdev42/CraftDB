#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "m-1.h"

static int testes_executados = 0;
static int testes_ok = 0;

static void verifica(const char *descricao, int condicao) {
    testes_executados++;
    if (condicao) {
        testes_ok++;
        printf("[OK]      %s\n", descricao);
    } else {
        printf("[FALHOU]  %s\n", descricao);
    }
}
#define VERIFICA(descricao, condicao) verifica(descricao, (condicao))

/* Remove o arquivo do banco para garantir um ambiente limpo a cada teste */
static void reinicia_banco(void) {
    remove(ARQUIVO_BD);
}

/* ---------------------------------------------------------------------
 * Teste 1: alocacao de paginas
 * --------------------------------------------------------------------- */
static void teste_alocacao_paginas(void) {
    printf("\n== Teste: alocacao de paginas ==\n");
    reinicia_banco();

    VERIFICA("banco comeca sem paginas alocadas", total_paginas_alocadas() == 0);

    int p0 = aloca_pagina();
    VERIFICA("primeira pagina alocada recebe numero 0", p0 == 0);
    VERIFICA("total de paginas alocadas passa a ser 1", total_paginas_alocadas() == 1);

    int p1 = aloca_pagina();
    int p2 = aloca_pagina();
    VERIFICA("paginas sao alocadas sequencialmente (numero 1)", p1 == 1);
    VERIFICA("paginas sao alocadas sequencialmente (numero 2)", p2 == 2);
    VERIFICA("total de paginas alocadas passa a ser 3", total_paginas_alocadas() == 3);
}

/* ---------------------------------------------------------------------
 * Teste 2: validacao de tamanho de buffer e numero de pagina
 * --------------------------------------------------------------------- */
static void teste_validacoes(void) {
    printf("\n== Teste: validacao de buffer e numero de pagina ==\n");
    reinicia_banco();
    aloca_pagina(); /* pagina 0 valida, usada nos testes de sucesso */

    unsigned char buffer_correto[TAMANHO_PAGINA];
    unsigned char buffer_pequeno[10];
    memset(buffer_correto, 0xAA, sizeof(buffer_correto));

    VERIFICA("escreve_pagina rejeita buffer menor que TAMANHO_PAGINA",
             escreve_pagina(0, buffer_pequeno, sizeof(buffer_pequeno)) != 0);

    VERIFICA("escreve_pagina rejeita buffer NULL",
             escreve_pagina(0, NULL, TAMANHO_PAGINA) != 0);

    VERIFICA("escreve_pagina rejeita numero de pagina negativo",
             escreve_pagina(-1, buffer_correto, sizeof(buffer_correto)) != 0);

    VERIFICA("escreve_pagina rejeita pagina ainda nao alocada",
             escreve_pagina(5, buffer_correto, sizeof(buffer_correto)) != 0);

    VERIFICA("escreve_pagina aceita pagina valida com buffer correto",
             escreve_pagina(0, buffer_correto, sizeof(buffer_correto)) == 0);

    VERIFICA("ler_pagina rejeita buffer menor que TAMANHO_PAGINA",
             ler_pagina(0, buffer_pequeno, sizeof(buffer_pequeno)) != 0);

    VERIFICA("ler_pagina rejeita numero de pagina negativo",
             ler_pagina(-1, buffer_correto, sizeof(buffer_correto)) != 0);

    VERIFICA("ler_pagina rejeita pagina nao alocada",
             ler_pagina(9, buffer_correto, sizeof(buffer_correto)) != 0);
}

/* ---------------------------------------------------------------------
 * Teste 3: escrita e leitura basicas (round-trip)
 * --------------------------------------------------------------------- */
static void teste_leitura_escrita(void) {
    printf("\n== Teste: escrita e leitura de pagina ==\n");
    reinicia_banco();
    aloca_pagina();

    unsigned char escrita[TAMANHO_PAGINA];
    unsigned char leitura[TAMANHO_PAGINA];
    memset(escrita, 0x5A, sizeof(escrita));
    memset(leitura, 0x00, sizeof(leitura));

    VERIFICA("escreve_pagina grava com sucesso",
             escreve_pagina(0, escrita, sizeof(escrita)) == 0);

    VERIFICA("ler_pagina le com sucesso",
             ler_pagina(0, leitura, sizeof(leitura)) == 0);

    VERIFICA("dados lidos sao identicos aos escritos",
             memcmp(escrita, leitura, TAMANHO_PAGINA) == 0);
}

/* ---------------------------------------------------------------------
 * Teste 4: isolamento entre paginas
 * --------------------------------------------------------------------- */
static void teste_isolamento_paginas(void) {
    printf("\n== Teste: isolamento entre paginas ==\n");
    reinicia_banco();
    aloca_pagina();
    aloca_pagina();
    aloca_pagina();

    unsigned char pagina0[TAMANHO_PAGINA];
    unsigned char conteudo_pagina1[TAMANHO_PAGINA];
    unsigned char pagina2[TAMANHO_PAGINA];
    unsigned char leitura[TAMANHO_PAGINA];

    memset(pagina0, 0x11, sizeof(pagina0));
    memset(conteudo_pagina1, 0x99, sizeof(conteudo_pagina1));
    memset(pagina2, 0x33, sizeof(pagina2));

    escreve_pagina(0, pagina0, sizeof(pagina0));
    escreve_pagina(1, conteudo_pagina1, sizeof(conteudo_pagina1));

    ler_pagina(0, leitura, sizeof(leitura));
    VERIFICA("escrever na pagina 1 nao altera a pagina 0",
             memcmp(leitura, pagina0, TAMANHO_PAGINA) == 0);

    escreve_pagina(2, pagina2, sizeof(pagina2));

    ler_pagina(1, leitura, sizeof(leitura));
    VERIFICA("escrever na pagina 2 nao altera a pagina 1",
             memcmp(leitura, conteudo_pagina1, TAMANHO_PAGINA) == 0);

    ler_pagina(2, leitura, sizeof(leitura));
    VERIFICA("pagina 2 contem exatamente o que foi escrito nela",
             memcmp(leitura, pagina2, TAMANHO_PAGINA) == 0);
}

/* ---------------------------------------------------------------------
 * Teste 5: sincronizacao de dados
 * --------------------------------------------------------------------- */
static void teste_sincronizacao(void) {
    printf("\n== Teste: sincronizacao de dados ==\n");
    reinicia_banco();
    aloca_pagina();

    VERIFICA("sincroniza_dados funciona com o arquivo existente",
             sincroniza_dados() == 0);
}

/* ---------------------------------------------------------------------
 * Teste: serializacao e desserializacao de registros (little-endian)
 * --------------------------------------------------------------------- */
static void teste_serializacao(void) {
    printf("\n== Teste: serializacao e desserializacao de registros ==\n");

    /* Round-trip simples, incluindo valor negativo */
    Registro original;
    original.id = 1000;
    original.valor = -12345;

    unsigned char bytes[TAMANHO_REGISTRO];
    Registro recuperado;

    serializa_registro(&original, bytes);
    desserializa_registro(bytes, &recuperado);

    VERIFICA("round-trip preserva o campo id",
             recuperado.id == original.id);
    VERIFICA("round-trip preserva o campo valor (numero negativo)",
             recuperado.valor == original.valor);

    /* Layout exato em little-endian, independente da CPU do host */
    Registro conhecido;
    conhecido.id = 0x11223344;
    conhecido.valor = 42; /* 0x2A */

    unsigned char esperado[TAMANHO_REGISTRO] = {
        0x44, 0x33, 0x22, 0x11,  /* id (0x11223344) em little-endian   */
        0x2A, 0x00, 0x00, 0x00   /* valor (42) em little-endian        */
    };
    unsigned char obtido[TAMANHO_REGISTRO];
    serializa_registro(&conhecido, obtido);

    VERIFICA("bytes gerados seguem exatamente o layout little-endian",
             memcmp(obtido, esperado, TAMANHO_REGISTRO) == 0);

    Registro lido_dos_bytes_esperados;
    desserializa_registro(esperado, &lido_dos_bytes_esperados);
    VERIFICA("desserializar bytes little-endian conhecidos reconstroi o registro certo",
             lido_dos_bytes_esperados.id == conhecido.id &&
             lido_dos_bytes_esperados.valor == conhecido.valor);

    /* Integracao: grava um registro serializado numa pagina de verdade e o releu */
    reinicia_banco();
    aloca_pagina();

    unsigned char pagina[TAMANHO_PAGINA];
    memset(pagina, 0, sizeof(pagina));
    memcpy(pagina + TAMANHO_CABECALHO, obtido, TAMANHO_REGISTRO);

    VERIFICA("pagina com registro serializado e gravada com sucesso",
             escreve_pagina(0, pagina, sizeof(pagina)) == 0);

    unsigned char pagina_lida[TAMANHO_PAGINA];
    ler_pagina(0, pagina_lida, sizeof(pagina_lida));

    Registro registro_do_disco;
    desserializa_registro(pagina_lida + TAMANHO_CABECALHO, &registro_do_disco);

    VERIFICA("registro lido do disco e desserializado corretamente",
             registro_do_disco.id == conhecido.id &&
             registro_do_disco.valor == conhecido.valor);
}

/* ---------------------------------------------------------------------
 * Teste 6: persistencia em disco entre execucoes (processos distintos)
 * --------------------------------------------------------------------- */

/* Modo auxiliar: quando chamado com --persistencia-fase2, este mesmo
 * executavel apenas le a pagina 2 e imprime seus primeiros 8 bytes em
 * hexadecimal. E' invocado como um PROCESSO NOVO, sem nenhum estado em
 * memoria herdado do processo que gravou os dados. */
static int modo_worker_persistencia(void) {
    unsigned char pagina[TAMANHO_PAGINA];
    if (ler_pagina(2, pagina, sizeof(pagina)) != 0) {
        return 1;
    }
    int i;
    for (i = 0; i < TAMANHO_REGISTRO; i++) {
        printf("%02x ", pagina[TAMANHO_CABECALHO + i]);
    }
    printf("\n");
    return 0;
}

static void teste_persistencia(const char *caminho_executavel) {
    printf("\n== Teste: persistencia entre execucoes ==\n");
    reinicia_banco();

    while (total_paginas_alocadas() <= 2) {
        aloca_pagina();
    }

    unsigned char registro[TAMANHO_REGISTRO] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04};
    unsigned char pagina[TAMANHO_PAGINA];

    ler_pagina(2, pagina, sizeof(pagina));
    memcpy(pagina + TAMANHO_CABECALHO, registro, TAMANHO_REGISTRO);

    VERIFICA("registro gravado com sucesso antes de encerrar o processo",
             escreve_pagina(2, pagina, sizeof(pagina)) == 0);

    /* Encerra este "processo de escrita" e inicia um processo TOTALMENTE
     * NOVO para verificar se os dados sobreviveram no arquivo em disco. */
    char comando[512];
    snprintf(comando, sizeof(comando), "%s --persistencia-fase2", caminho_executavel);

    FILE *processo_filho = popen(comando, "r");
    int ok = 0;
    if (processo_filho != NULL) {
        int valores[TAMANHO_REGISTRO];
        int lidos = fscanf(processo_filho, "%x %x %x %x %x %x %x %x",
                            &valores[0], &valores[1], &valores[2], &valores[3],
                            &valores[4], &valores[5], &valores[6], &valores[7]);
        pclose(processo_filho);

        if (lidos == TAMANHO_REGISTRO) {
            unsigned char lido[TAMANHO_REGISTRO];
            int i;
            for (i = 0; i < TAMANHO_REGISTRO; i++) {
                lido[i] = (unsigned char)valores[i];
            }
            ok = (memcmp(lido, registro, TAMANHO_REGISTRO) == 0);
        }
    }

    VERIFICA("um processo novo le, do disco, o registro gravado pelo processo anterior", ok);
}

/* ---------------------------------------------------------------------
 * Programa principal dos testes
 * --------------------------------------------------------------------- */
int main(int argc, char *argv[]) {
    if (argc == 2 && strcmp(argv[1], "--persistencia-fase2") == 0) {
        return modo_worker_persistencia();
    }

    teste_alocacao_paginas();
    teste_validacoes();
    teste_leitura_escrita();
    teste_isolamento_paginas();
    teste_serializacao();
    teste_sincronizacao();
    teste_persistencia(argv[0]);

    printf("\n============================================\n");
    printf("Resultado: %d/%d testes passaram\n", testes_ok, testes_executados);
    printf("============================================\n");

    reinicia_banco();

    return (testes_ok == testes_executados) ? 0 : 1;
}
