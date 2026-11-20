/* ============================================================
 *                 TABLE SCAN (VARREDURA COMPLETA)
 * ============================================================
 *
 * Operador de varredura sequencial de uma tabela. Percorre TODOS os registros
 * armazenados e entrega, um por vez, tuplas logicas (Row), no modelo iterador:
 *
 *      table_scan_abrir()  ->  table_scan_next() ... table_scan_next()  ->  table_scan_fechar()
 *
 * Esta versao NAO implementa WHERE: toda tupla valida e' entregue. Um filtro
 * pode ser aplicado por quem chama, sobre a Row devolvida por table_scan_next().
 *
 * Camadas utilizadas:
 *
 *   Catalog   -> catalogo_buscar_tabela() localiza a tabela pelo nome e
 *                selecionar_arquivo_tabela() aponta o Pager para o arquivo de dados
 *                dela (reiniciando o cache, que nao sabe de qual arquivo vem cada frame).
 *   BPlusTree -> metadados: a pagina 0 guarda a RAIZ (ler_raiz). Os dados ficam nas
 *                FOLHAS (NO_FOLHA), encadeadas pelo campo 'proxima'. O scan desce da
 *                raiz ate a folha mais a esquerda e segue a cadeia de folhas.
 *   Cache     -> fixar(n) abre a pagina n (pin) e soltar(n, 0) a libera. A varredura
 *                e' somente de leitura, entao a pagina nunca e' marcada como suja.
 *   Pager     -> total_paginas_alocadas() e desserializar_registro() (bytes -> Registro).
 *   Row       -> row_de_registro() monta a tupla logica a partir do Registro fisico.
 *
 * Layout de uma folha (definido por BPlusTree.c, serializar_no):
 *
 *   [ 0.. 3] tipo (NO_FOLHA)   [ 4.. 7] numero_chaves (slots ocupados, 0..MAX_CHAVES)
 *   [ 8..11] proxima folha ou SEM_PAGINA (-1)          [12..15] reservado
 *   [16..  ] MAX_CHAVES slots de TAMANHO_REGISTRO (8) bytes (id + valor)
 *   Slots ocupados: 0 .. numero_chaves-1. Os demais sao LIVRES e sao ignorados.
 *
 * Fluxo:
 *
 *   abrir : raiz -> desce por filhos[0] (sem manter pin) -> guarda a 1a folha
 *   next  : fixar(folha) ; para cada slot 0..MAX_CHAVES-1:
 *               livre  -> ignora
 *               valido -> desserializa, monta a Row, devolve
 *           esgotados os slots: soltar(folha, 0) e segue para 'proxima'
 *           proxima == SEM_PAGINA -> fim da varredura
 *
 * Politica de pins: a varredura mantem NO MAXIMO UM pin, na folha corrente. O pin
 * e' liberado ao avancar de folha, ao chegar no fim, em caso de erro e em
 * table_scan_fechar(). Enquanto uma varredura estiver parada no meio de uma folha,
 * o cache nao pode ser reiniciado (reiniciar_cache() recusa paginas fixadas) -
 * portanto, feche a varredura antes de inserir/buscar em outra tabela. Use UMA
 * varredura ativa por vez: o Pager trabalha com um unico arquivo corrente.
 *
 * ORDEM: crescente da chave (id), a mesma de row_percorrer().
 *
 * Convencao de retorno (igual ao Pager/Cache): erros em stderr.
 *   table_scan_abrir / table_scan_fechar : 0 = sucesso, -1 = erro
 *   table_scan_next                     : 1 = tupla entregue, 0 = fim da varredura, -1 = erro
 */

#ifndef TABLE_SCAN
#define TABLE_SCAN

/* Row.c ja inclui Pager.c, Cache.c, BPlusTree.c e Catalog.c (que traz o Parser).
   ROW_SEM_MAIN desliga o main de demonstracao de Row.c, para nao colidir com o
   main deste arquivo. */
#define ROW_SEM_MAIN
#include "Row.c"
#undef ROW_SEM_MAIN
/* Offsets do cabecalho da folha (espelham serializar_no/desserializar_no) */
#define SCAN_OFFSET_TIPO            0
#define SCAN_OFFSET_NUM_REGISTROS   4
#define SCAN_OFFSET_PROXIMA         8

/* ------------------------------------------------------------------ */
/* Estado da varredura                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    const TabelaCat *tabela;        /* metadados da tabela (pertence ao Catalogo)  */
    int              aberta;        /* 1 entre table_scan_abrir e table_scan_fechar  */
    int              terminada;     /* 1 quando nao ha mais registros / apos erro  */

    int              proxima_folha; /* proxima folha a abrir (SEM_PAGINA = nenhuma)*/
    int              folhas_abertas;/* protecao contra ciclo na cadeia de folhas   */
    int              pagina_atual;  /* folha fixada no momento (valida se fixada)  */
    int              fixada;        /* 1 se ha um pin ativo em pagina_atual        */
    unsigned char   *dados;         /* buffer da pagina no cache (valido c/ pin)   */
    int              proximo_slot;  /* proximo slot a examinar na folha corrente   */
    int              slots_ocupados;/* numero_chaves da folha corrente             */
} TableScan;

/* ------------------------------------------------------------------ */
/* Funcoes auxiliares internas                                         */
/* ------------------------------------------------------------------ */

/* Um slot e' valido (ocupado) se estiver entre os 'ocupados' primeiros da folha.
   Ponto unico de decisao sobre slots livres. */
static int slot_esta_ocupado(int slot, int slots_ocupados) {
    return slot < slots_ocupados;
}

/* Libera o pin da pagina corrente, se houver. Retorna 0 ou -1 (falha em soltar). */
static int table_scan_liberar_pagina(TableScan *scan) {
    int status = 0;
    if (scan->fixada) {
        /* varredura somente leitura: a pagina nunca e' marcada como suja */
        if (soltar(scan->pagina_atual, 0) != 0) {
            status = -1;
        }
        scan->fixada = 0;
        scan->dados = NULL;
    }
    return status;
}

/* Encerra a varredura apos um erro, sem deixar pin pendente. */
static int table_scan_falhar(TableScan *scan) {
    table_scan_liberar_pagina(scan);
    scan->terminada = 1;
    return -1;
}

/* Le a raiz nos metadados (pagina 0) e desce sempre por filhos[0] ate a folha
   mais a esquerda. ler_no() fixa, copia e solta, entao nenhum pin sobra.
   Retorna 0 e a pagina em *folha, ou -1 em erro. */
static int table_scan_localizar_primeira_folha(const TabelaCat *tabela, int *folha) {
    int pagina = ler_raiz();
    if (pagina < 0) {
        fprintf(stderr, "Erro: table scan nao conseguiu ler a raiz de '%s'\n", tabela->nome);
        return -1;
    }

    int limite = total_paginas_alocadas();   /* nenhum caminho valido passa disso */
    No no;
    for (int passos = 0; ; passos++) {
        if (passos > limite || pagina <= PAGINA_METADADOS || pagina >= limite) {
            fprintf(stderr, "Erro: arvore de '%s' corrompida (pagina %d invalida na descida)\n",
                    tabela->nome, pagina);
            return -1;
        }
        if (ler_no(pagina, &no) != 0) {
            return -1;
        }
        if (no.tipo == NO_FOLHA) {
            break;
        }
        pagina = no.filhos[0];
    }
    *folha = pagina;
    return 0;
}

/*
 Abre (fixa) a folha scan->proxima_folha e prepara a leitura de seus slots.
 Retorna 1 se uma folha foi aberta, 0 se a cadeia acabou, -1 em erro.
*/
static int table_scan_abrir_proxima_pagina(TableScan *scan) {
    if (scan->proxima_folha == SEM_PAGINA) {
        return 0;
    }

    int n = scan->proxima_folha;
    int total = total_paginas_alocadas();
    if (++scan->folhas_abertas > total || n <= PAGINA_METADADOS || n >= total) {
        fprintf(stderr, "Erro: cadeia de folhas de '%s' corrompida (pagina %d)\n",
                scan->tabela->nome, n);
        return -1;
    }

    unsigned char *pagina = fixar(n);
    if (pagina == NULL) {
        fprintf(stderr, "Erro: table scan nao conseguiu abrir a pagina %d de '%s'\n",
                n, scan->tabela->nome);
        return -1;
    }

    int32_t  tipo     = (int32_t)desserializar_uint32_little_endian(pagina + SCAN_OFFSET_TIPO);
    uint32_t ocupados = desserializar_uint32_little_endian(pagina + SCAN_OFFSET_NUM_REGISTROS);
    if (tipo != NO_FOLHA || ocupados > (uint32_t)MAX_CHAVES) {
        fprintf(stderr, "Erro: pagina %d de '%s' nao e' uma folha valida (tipo=%d, ocupados=%u)\n",
                n, scan->tabela->nome, (int)tipo, (unsigned)ocupados);
        soltar(n, 0);
        return -1;
    }

    scan->proxima_folha  = (int)(int32_t)desserializar_uint32_little_endian(pagina + SCAN_OFFSET_PROXIMA);
    scan->pagina_atual   = n;
    scan->fixada         = 1;
    scan->dados          = pagina;
    scan->proximo_slot   = 0;
    scan->slots_ocupados = (int)ocupados;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Interface publica                                                   */
/* ------------------------------------------------------------------ */

/*
 table_scan_abrir(scan, cat, nome_tabela)
 Prepara a varredura da tabela 'nome_tabela': consulta o catalogo, aponta o
 Pager para o arquivo de dados da tabela, le a raiz nos metadados e localiza a
 primeira folha. Nenhuma pagina fica fixada ate a primeira chamada de table_scan_next().
 O Catalogo deve continuar existindo (e nao ser movido) ate table_scan_fechar().
 Retorna 0 em caso de sucesso, ou -1 em caso de erro.
*/
int table_scan_abrir(TableScan *scan, Catalogo *cat, const char *nome_tabela) {
    if (scan == NULL || cat == NULL || nome_tabela == NULL) {
        fprintf(stderr, "Erro: table_scan_abrir recebeu argumento NULL\n");
        return -1;
    }
    memset(scan, 0, sizeof(*scan));
    scan->proxima_folha = SEM_PAGINA;

    const TabelaCat *tabela = catalogo_buscar_tabela(cat, nome_tabela);
    if (tabela == NULL) {
        fprintf(stderr, "Erro: tabela '%s' nao existe no catalogo\n", nome_tabela);
        return -1;
    }
    if (!selecionar_arquivo_tabela(cat, tabela)) {
        fprintf(stderr, "Erro: %s\n", cat->mensagem);
        return -1;
    }

    int primeira;
    if (table_scan_localizar_primeira_folha(tabela, &primeira) != 0) {
        return -1;
    }

    scan->tabela        = tabela;
    scan->proxima_folha = primeira;
    scan->aberta        = 1;
    return 0;
}

/*
 table_scan_next(scan, saida)
 Entrega em *saida a proxima tupla da tabela.
 - Percorre os slots da folha corrente, ignorando os livres.
 - Ao esgotar os slots, libera o pin da folha e avanca para a proxima.
 - So devolve "fim" (0) quando nao ha mais nenhum registro em nenhuma folha;
   nesse momento nenhum pin permanece ativo.
 Depois do fim (ou de um erro), novas chamadas continuam devolvendo 0.
 Retorna 1 se *saida recebeu uma tupla, 0 no fim da varredura, ou -1 em erro.
*/
int table_scan_next(TableScan *scan, Row *saida) {
    if (scan == NULL || saida == NULL) {
        fprintf(stderr, "Erro: table_scan_next recebeu argumento NULL\n");
        return -1;
    }
    if (!scan->aberta) {
        fprintf(stderr, "Erro: table_scan_next chamado em varredura nao aberta\n");
        return -1;
    }

    while (!scan->terminada) {
        /* 1) Sem folha corrente: abre a proxima da cadeia */
        if (!scan->fixada) {
            int r = table_scan_abrir_proxima_pagina(scan);
            if (r < 0) {
                return table_scan_falhar(scan);
            }
            if (r == 0) {
                scan->terminada = 1; /* cadeia acabou: fim real */
                break;
            }
        }

        /* 2) Percorre os slots da folha corrente */
        while (scan->proximo_slot < MAX_CHAVES) {
            int slot = scan->proximo_slot++;

            if (!slot_esta_ocupado(slot, scan->slots_ocupados)) {
                continue; /* slot livre: ignorado */
            }

            /* 3) Reconstroi a tupla: bytes -> Registro -> Row */
            Registro reg;
            desserializar_registro(scan->dados + OFFSET_REGISTROS + (size_t)slot * TAMANHO_REGISTRO,
                                   &reg);
            if (row_de_registro(saida, scan->tabela, &reg) != 0) {
                return table_scan_falhar(scan);
            }
            return 1;
        }

        /* 4) Slots esgotados: libera o pin e passa para a proxima folha */
        if (table_scan_liberar_pagina(scan) != 0) {
            return table_scan_falhar(scan);
        }
    }

    return 0; /* fim da varredura */
}

/*
 table_scan_fechar(scan)
 Encerra a varredura e libera qualquer pin ainda ativo (por exemplo, quando o
 chamador desiste antes do fim). Pode ser chamada mais de uma vez com seguranca.
 Retorna 0 em caso de sucesso, ou -1 em caso de erro.
*/
int table_scan_fechar(TableScan *scan) {
    if (scan == NULL) {
        return -1;
    }
    int status = table_scan_liberar_pagina(scan);
    scan->aberta = 0;
    scan->terminada = 1;
    return status;
}

/* ------------------------------------------------------------------ */
/* Demonstracao                                                        */
/* ------------------------------------------------------------------ */

/* Callback de row_percorrer: guarda os ids visitados, para conferir com o scan. */
typedef struct { int32_t ids[64]; int n; } ListaIds;

static int coletar_id_cb(const Row *row, void *arg) {
    ListaIds *lista = (ListaIds *)arg;
    int32_t id = 0;
    row_obter_int(row, 0, &id);
    if (lista->n < 64) lista->ids[lista->n++] = id;
    return 0;
}

#ifndef TABLE_SCAN_SEM_MAIN
int main(void) {
    static Catalogo cat;
    Coluna colunas[2];
    Row linha, tupla;
    TableScan scan;
    ListaIds via_scan = { {0}, 0 }, via_arvore = { {0}, 0 };
    int total, r;

    remove("alunos.dat");   /* parte de arquivos de dados limpos */
    remove("vazia.dat");
    catalogo_iniciar(&cat);

    /* 1) Catalogo: alunos (id INT, nome CHAR) e vazia (id INT, valor INT) */
    strcpy(colunas[0].nome, "id");    colunas[0].tipo = TOKEN_INT;
    strcpy(colunas[1].nome, "nome");  colunas[1].tipo = TOKEN_CHAR;
    if (!catalogo_adicionar_tabela(&cat, "alunos", colunas, 2)) {
        fprintf(stderr, "Erro no catalogo: %s\n", cat.mensagem);
        return 1;
    }
    strcpy(colunas[1].nome, "valor"); colunas[1].tipo = TOKEN_INT;
    if (!catalogo_adicionar_tabela(&cat, "vazia", colunas, 2)) {
        fprintf(stderr, "Erro no catalogo: %s\n", cat.mensagem);
        return 1;
    }
    const TabelaCat *alunos = catalogo_buscar_tabela(&cat, "alunos");

    /* 2) Carga fora de ordem: com 3 slots por folha, forca varias divisoes */
    const int32_t ids[]   = { 50, 10, 80, 30, 70, 20, 60, 40, 90, 100 };
    const char   *nomes[] = { "Eva", "Ana", "Iris", "Caio", "Hugo", "Bia", "Gil", "Davi", "Joao", "Kai" };
    const int quantidade = (int)(sizeof(ids) / sizeof(ids[0]));
    for (int i = 0; i < quantidade; i++) {
        row_iniciar(&linha, alunos);
        row_definir_int(&linha, 0, ids[i]);
        row_definir_char(&linha, 1, nomes[i]);
        if (row_inserir(&cat, &linha) != INSERE_OK) {
            fprintf(stderr, "Falha ao inserir a linha %d\n", i);
            return 1;
        }
    }
    printf("%d linhas inseridas em 'alunos' (fora de ordem).\n\n", quantidade);

    /* 3) Varredura completa de 'alunos' */
    printf("Table scan de 'alunos':\n");
    if (table_scan_abrir(&scan, &cat, "alunos") != 0) return 1;
    total = 0;
    while ((r = table_scan_next(&scan, &tupla)) == 1) {
        int32_t id = 0;
        printf("  ");
        row_imprimir(&tupla);
        row_obter_int(&tupla, 0, &id);
        if (via_scan.n < 64) via_scan.ids[via_scan.n++] = id;
        total++;
    }
    if (r < 0) {
        fprintf(stderr, "Erro durante a varredura\n");
        table_scan_fechar(&scan);
        return 1;
    }
    printf("  %d tupla(s) entregue(s); fim da varredura.\n", total);
    printf("  next() apos o fim -> %d (esperado 0)\n", table_scan_next(&scan, &tupla));
    table_scan_fechar(&scan);
    imprimir_estatisticas_cache();

    /* 3b) Confere com o percurso da propria arvore B+ (row_percorrer) */
    row_percorrer(&cat, "alunos", coletar_id_cb, &via_arvore);
    printf("  conferencia com row_percorrer(): %s\n",
           (via_scan.n == quantidade && via_scan.n == via_arvore.n &&
            memcmp(via_scan.ids, via_arvore.ids, sizeof(int32_t) * (size_t)via_scan.n) == 0)
               ? "identico (esperado)" : "DIVERGENTE");

    /* 4) Tabela vazia: fim imediato, sem nenhuma tupla */
    printf("\nTable scan de 'vazia':\n");
    if (table_scan_abrir(&scan, &cat, "vazia") != 0) return 1;
    total = 0;
    while ((r = table_scan_next(&scan, &tupla)) == 1) total++;
    printf("  %d tupla(s); next() final -> %d (esperado 0)\n", total, r);
    table_scan_fechar(&scan);

    /* 5) Desistencia no meio: fechar libera o pin (o cache volta a poder ser reiniciado) */
    printf("\nInterrompendo a varredura de 'alunos' apos a 1a tupla:\n");
    if (table_scan_abrir(&scan, &cat, "alunos") != 0) return 1;
    if (table_scan_next(&scan, &tupla) == 1) {
        printf("  recebida: ");
        row_imprimir(&tupla);
    }
    printf("  reiniciar_cache() com varredura aberta -> %s\n",
           reiniciar_cache() != 0 ? "recusado (pagina fixada)" : "aceito");
    table_scan_fechar(&scan);
    printf("  reiniciar_cache() apos table_scan_fechar() -> %s\n",
           reiniciar_cache() == 0 ? "aceito (pin liberado)" : "recusado (pin vazado!)");

    /* 6) Tabela inexistente */
    printf("\nAbrir tabela inexistente:\n  ");
    if (table_scan_abrir(&scan, &cat, "fantasma") != 0) printf("  -> rejeitado (esperado)\n");

    remove("alunos.dat");
    remove("vazia.dat");
    return 0;
}

#endif /* TABLE_SCAN_SEM_MAIN */

#endif /* TABLE_SCAN */
