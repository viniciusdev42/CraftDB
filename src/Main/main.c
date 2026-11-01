#ifndef DB_CRAFT_MAIN
#define DB_CRAFT_MAIN

#include "Pager.c"
#include "Cache.c"
#include "BPlusTree.c"
#include "Tokenizer.c"
#include "Parser.c"

int main(void) {

    printf("\n--- Alocacao de Paginas ---\n");
    
    int pagina_alvo = 2;
    int slot_alvo = 0;

    /* Garante que as paginas 0, 1 e 2 existam antes de usar a pagina 2 */
    while (total_paginas_alocadas() <= pagina_alvo) {
        if (aloca_pagina() < 0) {
            fprintf(stderr, "Erro Ao Alocar Paginas Necessarias.\n");
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

    printf("O Registro Começa No Byte: %ld\n", byte_absoluto);

    /* Serializa e desserializa um registro */

    Registro registro_bruto = { .id = 1, .valor = 20260001 };
    
    unsigned char registro_bytes[TAMANHO_REGISTRO];
    
    serializa_registro(&registro_bruto,registro_bytes);

    unsigned char pagina_lida[TAMANHO_PAGINA];
    
    Registro registro_lido;
    
    ler_pagina(pagina_alvo, pagina_lida, sizeof(pagina_lida));
    
    desserializa_registro(pagina_lida + offset_na_pagina, &registro_lido);
    
    printf("Registro Relido Do Disco e Desserializado: ID=%d, Valor=%d\n",
           registro_lido.id, registro_lido.valor);


     printf("\n ----- Cache de Paginas ----- \n");

    /* Garante paginas suficientes para forcar expulsao (mais paginas que frames) */
    while (total_paginas_alocadas() < NUM_FRAMES + 2) {
        if (aloca_pagina() < 0) {
            fprintf(stderr, "Erro Ao Alocar Paginas Extras Para O Teste Do Cache.\n");
            return 1;
        }
    }

    /* fixa(2): primeira vez -> falta (miss) */
    unsigned char *buf2 = fixa(pagina_alvo);
    if (buf2 == NULL) return 1;

    Registro lido_via_cache;
    
    desserializa_registro(buf2 + offset_na_pagina, &lido_via_cache);
    
    printf("[CACHE] Pagina %d Fixada -> ID=%d, Valor=%d\n",
           pagina_alvo, lido_via_cache.id, lido_via_cache.valor);

    /* Modifica o registro diretamente no buffer do cache e marca a pagina como suja */
    
    Registro atualizado = { .id = 99, .valor = 12345 };
    
    unsigned char atualizado_bytes[TAMANHO_REGISTRO];
    
    serializa_registro(&atualizado, atualizado_bytes);
    
    memcpy(buf2 + offset_na_pagina, atualizado_bytes, TAMANHO_REGISTRO);

    solta(pagina_alvo, 1); /* solta marcando suja; NO-FORCE: nao grava ainda */

    /* fixa(2) novamente: deve ser acerto (hit) e devolver a MESMA referencia */
    unsigned char *buf2_de_novo = fixa(pagina_alvo);
    
    printf("[CACHE] Segunda Fixacao Da Pagina %d -> Mesma Referencia? %s\n",
           pagina_alvo, (buf2 == buf2_de_novo) ? "Sim" : "Nao");
    
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
    
    printf("[CACHE] Apos Possivel Expulsao, Disco Contem ID=%d, Valor=%d (Esperado ID=99, Valor=12345)\n",
           verificado.id, verificado.valor);

    /* descarrega() explicito: garante que qualquer pagina ainda suja no cache
       seja persistida (uso tipico ao final do programa) */
    for (int p = 0; p < total_paginas_alocadas(); p++) {
        descarrega(p);
    }
    
    printf("\n ---- Arvore B+ -----\n");

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
        fprintf(stderr, "Erro Ao Inicializar A B+ Tree.\n");
        return 1;
    }

    int32_t chaves[] = { 35, 45, 50, 55 };
    
    int n = (int)(sizeof(chaves) / sizeof(chaves[0]));

    printf("\n ---- Insercoes na B+ Tree (MAXIMO: %d CHAVES POR NO) ----- \n", MAX_CHAVES);
    
    for (int i = 0; i < n; i++) {
        Registro reg = { .id = chaves[i], .valor = chaves[i] * 10 };
        int r = inserir_valor(&reg);
        if (r == INSERE_DUPLICADO) {
            printf("\n Chave %d Ja Existe.\n", chaves[i]);
            continue;
        }
        if (r != INSERE_OK) {
            fprintf(stderr, "Erro Ao Inserir a Chave %d\n", chaves[i]);
            return 1;
        }
        printf("\n Apos Inserir %d:\n", chaves[i]);
        imprimir_arvore();
    }

    printf("\n ------ Busca B + Tree --------\n");
    
    int32_t buscas[] = { 35, 50, 55, 40 };
    
    for (int i = 0; i < 4; i++) {
        Registro achado;
        int r = buscar_valor(buscas[i], &achado);
        if (r == 0) {
            printf("Busca(%d): Encontrado -> ID=%d, Valor=%d\n", buscas[i], achado.id, achado.valor);
        } else if (r == 1) {
            printf("Busca(%d): Nao Encontrado\n", buscas[i]);
        } else {
            printf("Busca(%d): Erro\n", buscas[i]);
        }
    }

    /* Persiste todas as paginas ainda sujas no cache (NO-FORCE) */

    for (int p = 0; p < total_paginas_alocadas(); p++) {
        descarrega(p);
    }

    sincroniza_dados();
    
    printf("\n ----- TOKENIZADOR ---- \n\n");
    
    static const char *EXEMPLO =
      "CREATE TABLE alunos (id INT, nome CHAR);\n"
      "INSERT INTO alunos VALUES (1, 'Ana');\n"
      "SELECT * FROM alunos WHERE id >= 1 AND nome <> 'Bia';";

    char *arquivo = NULL;
    const char *texto = EXEMPLO;

    Tokenizador t;
    iniciar_tokenizador(&t, texto);

    Parser parser;
    iniciar_parser(&parser, texto);

    printf("%-4s %-14s %-8s %s\n", "#", "TOKEN", "POSICAO", "LEXEMA");
    printf("\n --------------------------------------------------\n");

    int ordem = 0;
    int status = 0;
    int erros =0 ;
    
    
    Token token;
    do {
        token = proximo_token(&t);
        printf("%-4d %-14s %d:%-5d '%s'\n",
               ++ordem, nome_token(token.tipo), token.linha, token.coluna, token.lexema);

        if (token.tipo == TOKEN_ERRO || token.tipo == TOKEN_NAO_SUPORTADO)
            status = 1;
    } while (token.tipo != TOKEN_FIM);
    
    printf("\n ------- PARSER ------ \n\n");
    
    while (parser.atual.tipo != TOKEN_FIM) {
        Comando comando;

        if (parse_comando(&parser, &comando)) {
            printf("[COMANDO %d] ", ++ordem);
            imprimir_comando(&comando);
        } else {
            printf("[COMANDO %d] Erro Sintatico\n  %s\n", ++ordem, parser.mensagem);
            erros++;
            sincronizar(&parser);
        }
        printf("\n");
    }

    if (erros)
        printf("Analise Concluida Com %d Erro(s)\n", erros);
    else
        printf("Analise Concluida Sem Erros\n");

    free(arquivo);
    
    return status;
    
    return erros ? 1 : 0;
    
    return 0;
}
#endif
