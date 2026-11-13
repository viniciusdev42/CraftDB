/* ============================================================
 *                 ROW (LINHA / TUPLA LOGICA)
 * ============================================================
 *
 * Uma Row e' a representacao LOGICA de uma linha de uma tabela: uma lista de
 * campos (nome, tipo, valor), na ordem das colunas definidas no Catalogo.
 * Ela NAO conhece bytes, paginas, offsets nem o formato fisico de 8 bytes do
 * Registro: esse conhecimento fica isolado nas duas funcoes de ponte
 * (row_de_registro / row_para_registro).
 *
 * Camadas utilizadas:
 *
 *   Catalog     -> define colunas e tipos (INT / CHAR) e o tamanho de cada coluna.
 *                  A Row copia esses metadados ao ser criada (row_iniciar), por
 *                  isso continua valida mesmo que o catalogo seja alterado/movido.
 *   BPlusTree   -> armazena os Registros fisicos (inserir_valor, buscar_valor,
 *                  percorrer_registros).
 *   Cache/Pager -> acessados indiretamente, via Catalog (selecionar_arquivo_tabela
 *                  reinicia o cache e troca o arquivo) e diretamente para
 *                  persistir (descarregar_todo_cache, sincronizar_dados).
 *
 * Acesso aos valores:
 *   - por POSICAO da coluna (base 0):       row_obter_int(row, 0, &v)
 *   - por IDENTIFICADOR (nome da coluna):   row_obter_int_por_nome(row, "id", &v)
 *   Nomes nao diferenciam maiusculas/minusculas (mesma regra do Catalogo).
 *   O tipo definido no catalogo e' preservado: ler/gravar uma coluna INT como
 *   CHAR (ou vice-versa) e' rejeitado com erro.
 *
 * Convencao de retorno (igual ao Pager/Cache): 0 = sucesso, -1 = erro, com a
 * mensagem em stderr. Excecoes documentadas: row_buscar (0/1/-1, como
 * buscar_valor) e row_inserir (codigos INSERE_* da arvore B+).
 *
 * Observacao de compilacao: o Parser nao e' parte deste arquivo, mas o Catalog.c
 * depende dele (tipos Coluna, Comando, TipoToken, contexto_igual, nome_token...).
 */

#ifndef ROW
#define ROW

#include "Pager.c"
#include "Cache.c"
#include "BPlusTree.c"
#include "Catalog.c"

/* ------------------------------------------------------------------ */
/* Estruturas                                                          */
/* ------------------------------------------------------------------ */

/* Um campo = coluna (nome, tipo, tamanho) + valor logico.
   Somente o membro correspondente ao 'tipo' e' significativo. */
typedef struct {
    char      nome[MAX_LEXEMA];     /* nome da coluna (copiado do catalogo)  */
    TipoToken tipo;                 /* TOKEN_INT ou TOKEN_CHAR               */
    int       tamanho;              /* bytes da coluna (limite do CHAR)      */
    int32_t   inteiro;              /* valor, se tipo == TOKEN_INT           */
    char      texto[TAM_CHAR + 1];  /* valor, se tipo == TOKEN_CHAR          */
} CampoRow;

typedef struct {
    char     tabela[MAX_LEXEMA];    /* tabela a que a linha pertence         */
    CampoRow campos[MAX_COLUNAS];
    int      numero_campos;
} Row;

/* ------------------------------------------------------------------ */
/* Criacao e resolucao de colunas                                      */
/* ------------------------------------------------------------------ */

/*
 row_iniciar(row, tabela)
 Cria uma linha vazia com o esquema da tabela do catalogo: cada campo recebe
 nome, tipo e tamanho da coluna correspondente; valores iniciam em 0 / "".
 Retorna 0 em caso de sucesso, ou -1 se algum argumento for NULL.
*/
int row_iniciar(Row *row, const TabelaCat *tabela) {
    if (row == NULL || tabela == NULL) {
        fprintf(stderr, "Erro: row_iniciar recebeu argumento NULL\n");
        return -1;
    }

    memset(row, 0, sizeof(*row));
    strcpy(row->tabela, tabela->nome);
    row->numero_campos = tabela->numero_colunas;

    for (int i = 0; i < tabela->numero_colunas; i++) {
        strcpy(row->campos[i].nome, tabela->colunas[i].nome);
        row->campos[i].tipo    = tabela->colunas[i].tipo;
        row->campos[i].tamanho = tabela->colunas[i].tamanho;
    }
    return 0;
}

/* Devolve a posicao (base 0) da coluna de nome 'nome', ou -1 se nao existir. */
int row_posicao_da_coluna(const Row *row, const char *nome) {
    if (row == NULL || nome == NULL) {
        return -1;
    }
    for (int i = 0; i < row->numero_campos; i++) {
        if (contexto_igual(row->campos[i].nome, nome)) {
            return i;
        }
    }
    return -1;
}

/* Retorna o campo da posicao pos, ou NULL (com mensagem) se a posicao for invalida. */
static const CampoRow *row_campo(const Row *row, int pos) {
    if (row == NULL) {
        fprintf(stderr, "Erro: row NULL\n");
        return NULL;
    }
    if (pos < 0 || pos >= row->numero_campos) {
        fprintf(stderr, "Erro: posicao de coluna invalida (%d) na tabela '%s' (%d coluna(s))\n",
                pos, row->tabela, row->numero_campos);
        return NULL;
    }
    return &row->campos[pos];
}

/* Versao mutavel de row_campo (mesma validacao). */
static CampoRow *row_campo_mutavel(Row *row, int pos) {
    return (CampoRow *)row_campo(row, pos);
}

/* Resolve nome -> posicao, emitindo erro se a coluna nao existir. */
static int row_resolve_nome(const Row *row, const char *nome) {
    int pos = row_posicao_da_coluna(row, nome);
    if (pos < 0) {
        fprintf(stderr, "Erro: coluna '%s' nao existe na tabela '%s'\n",
                nome != NULL ? nome : "(null)", row != NULL ? row->tabela : "?");
    }
    return pos;
}

/* Tipo (TOKEN_INT / TOKEN_CHAR) da coluna na posicao pos, via *saida. */
int row_tipo_da_coluna(const Row *row, int pos, TipoToken *saida) {
    const CampoRow *campo = row_campo(row, pos);
    if (campo == NULL || saida == NULL) {
        return -1;
    }
    *saida = campo->tipo;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Leitura de valores (por posicao e por nome)                         */
/* ------------------------------------------------------------------ */

/* Le o valor INT da coluna na posicao pos. Erro se a coluna for CHAR. */
int row_obter_int(const Row *row, int pos, int32_t *saida) {
    const CampoRow *campo = row_campo(row, pos);
    if (campo == NULL || saida == NULL) {
        return -1;
    }
    if (campo->tipo != TOKEN_INT) {
        fprintf(stderr, "Erro: a coluna '%s' da tabela '%s' e CHAR, nao INT\n",
                campo->nome, row->tabela);
        return -1;
    }
    *saida = campo->inteiro;
    return 0;
}

/* Devolve o texto da coluna CHAR na posicao pos (ponteiro interno da Row, valido
   enquanto a Row existir), ou NULL se a posicao for invalida ou a coluna for INT. */
const char *row_obter_char(const Row *row, int pos) {
    const CampoRow *campo = row_campo(row, pos);
    if (campo == NULL) {
        return NULL;
    }
    if (campo->tipo != TOKEN_CHAR) {
        fprintf(stderr, "Erro: a coluna '%s' da tabela '%s' e INT, nao CHAR\n",
                campo->nome, row->tabela);
        return NULL;
    }
    return campo->texto;
}

int row_obter_int_por_nome(const Row *row, const char *coluna, int32_t *saida) {
    int pos = row_resolve_nome(row, coluna);
    return (pos < 0) ? -1 : row_obter_int(row, pos, saida);
}

const char *row_obter_char_por_nome(const Row *row, const char *coluna) {
    int pos = row_resolve_nome(row, coluna);
    return (pos < 0) ? NULL : row_obter_char(row, pos);
}

/* ------------------------------------------------------------------ */
/* Escrita de valores (por posicao e por nome)                         */
/* ------------------------------------------------------------------ */

/* Define o valor INT da coluna na posicao pos. Erro se a coluna for CHAR. */
int row_definir_int(Row *row, int pos, int32_t valor) {
    CampoRow *campo = row_campo_mutavel(row, pos);
    if (campo == NULL) {
        return -1;
    }
    if (campo->tipo != TOKEN_INT) {
        fprintf(stderr, "Erro: a coluna '%s' da tabela '%s' e CHAR; nao aceita valor INT\n",
                campo->nome, row->tabela);
        return -1;
    }
    campo->inteiro = valor;
    return 0;
}

/* Define o valor CHAR da coluna na posicao pos. Erro se a coluna for INT ou se
   o texto exceder o tamanho da coluna (mesma regra de valor_compativel do catalogo). */
int row_definir_char(Row *row, int pos, const char *texto) {
    CampoRow *campo = row_campo_mutavel(row, pos);
    if (campo == NULL || texto == NULL) {
        return -1;
    }
    if (campo->tipo != TOKEN_CHAR) {
        fprintf(stderr, "Erro: a coluna '%s' da tabela '%s' e INT; nao aceita valor CHAR\n",
                campo->nome, row->tabela);
        return -1;
    }
    if (strlen(texto) > (size_t)campo->tamanho) {
        fprintf(stderr, "Erro: '%s' tem %d caracteres, mas a coluna '%s' comporta %d\n",
                texto, (int)strlen(texto), campo->nome, campo->tamanho);
        return -1;
    }
    strcpy(campo->texto, texto);
    return 0;
}

int row_definir_int_por_nome(Row *row, const char *coluna, int32_t valor) {
    int pos = row_resolve_nome(row, coluna);
    return (pos < 0) ? -1 : row_definir_int(row, pos, valor);
}

int row_definir_char_por_nome(Row *row, const char *coluna, const char *texto) {
    int pos = row_resolve_nome(row, coluna);
    return (pos < 0) ? -1 : row_definir_char(row, pos, texto);
}

/* ------------------------------------------------------------------ */
/* Ponte Row <-> Registro fisico (unico ponto que conhece o formato)   */
/* ------------------------------------------------------------------ */

/*
 row_para_registro(row, reg)
 Converte a Row para o Registro de 8 bytes usado pela arvore B+:
   coluna 0 (INT)          -> reg->id (chave)
   coluna 1 (INT ou CHAR)  -> reg->valor (CHAR e' empacotado em 4 bytes)
 Segue as mesmas restricoes do catalogo (1a coluna INT, no maximo 2 colunas).
 Retorna 0 em caso de sucesso, ou -1 em caso de erro.
*/
int row_para_registro(const Row *row, Registro *reg) {
    if (row == NULL || reg == NULL) {
        fprintf(stderr, "Erro: row_para_registro recebeu argumento NULL\n");
        return -1;
    }
    if (row->numero_campos < 1 || row->numero_campos > 2 || row->campos[0].tipo != TOKEN_INT) {
        fprintf(stderr, "Erro: a Row da tabela '%s' nao e' compativel com o formato da arvore B+ "
                        "(1a coluna INT e no maximo 2 colunas)\n", row->tabela);
        return -1;
    }

    reg->id    = row->campos[0].inteiro;
    reg->valor = 0;

    if (row->numero_campos == 2) {
        const CampoRow *campo = &row->campos[1];
        if (campo->tipo == TOKEN_INT) {
            reg->valor = campo->inteiro;
        } else if (!empacotar_char(campo->texto, &reg->valor)) {
            fprintf(stderr, "Erro: CHAR da coluna '%s' nao cabe no formato fisico de 4 bytes\n",
                    campo->nome);
            return -1;
        }
    }
    return 0;
}

/*
 row_de_registro(row, tabela, reg)
 Monta uma Row (esquema da tabela) a partir de um Registro lido da arvore B+,
 desempacotando o CHAR quando a 2a coluna for desse tipo.
 Retorna 0 em caso de sucesso, ou -1 em caso de erro.
*/
int row_de_registro(Row *row, const TabelaCat *tabela, const Registro *reg) {
    if (reg == NULL || row_iniciar(row, tabela) != 0) {
        return -1;
    }
    if (tabela->numero_colunas < 1 || tabela->numero_colunas > 2 ||
        tabela->colunas[0].tipo != TOKEN_INT) {
        fprintf(stderr, "Erro: a tabela '%s' nao e' compativel com o formato da arvore B+\n",
                tabela->nome);
        return -1;
    }

    row->campos[0].inteiro = reg->id;

    if (tabela->numero_colunas == 2) {
        if (tabela->colunas[1].tipo == TOKEN_INT) {
            row->campos[1].inteiro = reg->valor;
        } else {
            desempacotar_char(reg->valor, row->campos[1].texto);
        }
    }
    return 0;
}

/* Confere se o esquema da Row ainda coincide com o da tabela do catalogo. */
static int row_confere_esquema(const Row *row, const TabelaCat *tabela) {
    if (row->numero_campos != tabela->numero_colunas) {
        return 0;
    }
    for (int i = 0; i < row->numero_campos; i++) {
        if (row->campos[i].tipo != tabela->colunas[i].tipo ||
            !contexto_igual(row->campos[i].nome, tabela->colunas[i].nome)) {
            return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Persistencia via arvore B+ / cache / pager                          */
/* ------------------------------------------------------------------ */

/*
 row_inserir(cat, row)
 Grava a Row na arvore B+ da sua tabela e forca a persistencia em disco
 (descarregar_todo_cache + sincronizar_dados).
 Retorna INSERE_OK, INSERE_DUPLICADO (chave ja existe) ou INSERE_ERRO.
*/
int row_inserir(Catalogo *cat, const Row *row) {
    if (cat == NULL || row == NULL) {
        fprintf(stderr, "Erro: row_inserir recebeu argumento NULL\n");
        return INSERE_ERRO;
    }

    const TabelaCat *tabela = catalogo_buscar_tabela(cat, row->tabela);
    if (tabela == NULL) {
        fprintf(stderr, "Erro: tabela '%s' nao existe no catalogo\n", row->tabela);
        return INSERE_ERRO;
    }
    if (!row_confere_esquema(row, tabela)) {
        fprintf(stderr, "Erro: o esquema da Row nao confere com o da tabela '%s' no catalogo\n",
                tabela->nome);
        return INSERE_ERRO;
    }

    Registro reg;
    if (row_para_registro(row, &reg) != 0) {
        return INSERE_ERRO;
    }
    if (!selecionar_arquivo_tabela(cat, tabela)) {
        fprintf(stderr, "Erro: %s\n", cat->mensagem);
        return INSERE_ERRO;
    }

    int r = inserir_valor(&reg);
    if (r != INSERE_OK) {
        return r;
    }
    if (descarregar_todo_cache() != 0 || sincronizar_dados() != 0) {
        fprintf(stderr, "Erro: falha ao persistir a linha na tabela '%s'\n", tabela->nome);
        return INSERE_ERRO;
    }
    return INSERE_OK;
}

/*
 row_buscar(cat, nome_tabela, chave, saida)
 Busca na arvore B+ a linha cuja primeira coluna (chave) vale 'chave' e a
 devolve como Row em *saida.
 Retorna 0 se encontrou, 1 se nao existe, ou -1 em caso de erro.
*/
int row_buscar(Catalogo *cat, const char *nome_tabela, int32_t chave, Row *saida) {
    if (cat == NULL || nome_tabela == NULL || saida == NULL) {
        fprintf(stderr, "Erro: row_buscar recebeu argumento NULL\n");
        return -1;
    }

    const TabelaCat *tabela = catalogo_buscar_tabela(cat, nome_tabela);
    if (tabela == NULL) {
        fprintf(stderr, "Erro: tabela '%s' nao existe no catalogo\n", nome_tabela);
        return -1;
    }
    if (!selecionar_arquivo_tabela(cat, tabela)) {
        fprintf(stderr, "Erro: %s\n", cat->mensagem);
        return -1;
    }

    Registro reg;
    int r = buscar_valor(chave, &reg);
    if (r != 0) {
        return r;   /* 1 = nao encontrada, -1 = erro */
    }
    return row_de_registro(saida, tabela, &reg);
}

/* Contexto que adapta o callback de Registro (arvore) para o de Row (usuario). */
typedef struct {
    const TabelaCat *tabela;
    int (*callback)(const Row *, void *);
    void *arg;
    int erro;
} ContextoRow;

static int adaptador_percorrer(const Registro *reg, void *arg) {
    ContextoRow *ctx = (ContextoRow *)arg;
    Row row;
    if (row_de_registro(&row, ctx->tabela, reg) != 0) {
        ctx->erro = 1;
        return 1;   /* interrompe o percurso */
    }
    return ctx->callback(&row, ctx->arg);
}

/*
 row_percorrer(cat, nome_tabela, cb, arg)
 Visita todas as linhas da tabela em ordem crescente da chave, chamando
 cb(row, arg). Se cb devolver valor != 0, o percurso para antes do fim.
 Retorna o numero de linhas visitadas (>= 0), ou -1 em caso de erro.
*/
int row_percorrer(Catalogo *cat, const char *nome_tabela,
                  int (*cb)(const Row *, void *), void *arg) {
    if (cat == NULL || nome_tabela == NULL || cb == NULL) {
        fprintf(stderr, "Erro: row_percorrer recebeu argumento NULL\n");
        return -1;
    }

    const TabelaCat *tabela = catalogo_buscar_tabela(cat, nome_tabela);
    if (tabela == NULL) {
        fprintf(stderr, "Erro: tabela '%s' nao existe no catalogo\n", nome_tabela);
        return -1;
    }
    if (!selecionar_arquivo_tabela(cat, tabela)) {
        fprintf(stderr, "Erro: %s\n", cat->mensagem);
        return -1;
    }

    ContextoRow ctx = { tabela, cb, arg, 0 };
    int visitadas = percorrer_registros(adaptador_percorrer, &ctx);
    return ctx.erro ? -1 : visitadas;
}

/* ------------------------------------------------------------------ */
/* Impressao                                                           */
/* ------------------------------------------------------------------ */

/* Imprime a linha no formato: tabela(coluna=valor, ...) */
void row_imprimir(const Row *row) {
    if (row == NULL) {
        return;
    }
    printf("%s(", row->tabela);
    for (int i = 0; i < row->numero_campos; i++) {
        const CampoRow *c = &row->campos[i];
        if (i) printf(", ");
        if (c->tipo == TOKEN_INT) printf("%s=%d", c->nome, c->inteiro);
        else                      printf("%s='%s'", c->nome, c->texto);
    }
    printf(")\n");
}

/* ------------------------------------------------------------------ */
/* Demonstracao                                                        */
/* ------------------------------------------------------------------ */

#ifndef ROW_SEM_MAIN

static int imprime_linha_cb(const Row *row, void *arg) {
    (void)arg;
    printf("  ");
    row_imprimir(row);
    return 0;
}

int main(void) {
    static Catalogo cat;
    Coluna colunas[2];
    Row linha, lida;
    int32_t id = 0;

    remove("alunos.dat");   /* parte de um arquivo de dados limpo */
    catalogo_iniciar(&cat);

    /* 1) Catalogo: alunos (id INT, nome CHAR) */
    strcpy(colunas[0].nome, "id");   colunas[0].tipo = TOKEN_INT;
    strcpy(colunas[1].nome, "nome"); colunas[1].tipo = TOKEN_CHAR;
    if (!catalogo_adicionar_tabela(&cat, "alunos", colunas, 2)) {
        fprintf(stderr, "Erro no catalogo: %s\n", cat.mensagem);
        return 1;
    }
    const TabelaCat *tabela = catalogo_buscar_tabela(&cat, "alunos");

    /* 2) Monta linhas logicas, acessando colunas por nome e por posicao */
    const char *nomes[] = { "Ana", "Bia", "Caio", "Davi", "Eva" };
    for (int i = 0; i < 5; i++) {
        if (row_iniciar(&linha, tabela) != 0) return 1;
        if (i % 2 == 0) {
            row_definir_int_por_nome(&linha, "ID", 10 * (i + 1));  /* por nome (sem case) */
            row_definir_char_por_nome(&linha, "nome", nomes[i]);
        } else {
            row_definir_int(&linha, 0, 10 * (i + 1));              /* por posicao */
            row_definir_char(&linha, 1, nomes[i]);
        }
        if (row_inserir(&cat, &linha) != INSERE_OK) {
            fprintf(stderr, "Falha ao inserir a linha %d\n", i);
            return 1;
        }
    }
    printf("5 linhas inseridas.\n");

    /* 3) Chave duplicada e' reportada pela arvore B+ */
    row_iniciar(&linha, tabela);
    row_definir_int(&linha, 0, 10);
    row_definir_char(&linha, 1, "Dup");
    printf("Inserir id=10 de novo -> %s\n",
           row_inserir(&cat, &linha) == INSERE_DUPLICADO ? "INSERE_DUPLICADO (esperado)" : "inesperado");

    /* 4) Leitura por chave, com acesso por nome e por posicao */
    if (row_buscar(&cat, "alunos", 30, &lida) == 0) {
        const char *nome = row_obter_char_por_nome(&lida, "nome");
        printf("Busca id=30 -> ");
        row_imprimir(&lida);
        row_obter_int(&lida, 0, &id);
        printf("  coluna 0 (INT) = %d | coluna 'nome' (CHAR) = '%s'\n", id, nome);
    }
    printf("Busca id=99 -> %s\n", row_buscar(&cat, "alunos", 99, &lida) == 1 ? "nao encontrada" : "inesperado");

    /* 5) Tipos do catalogo sao preservados: acesso com tipo errado e' rejeitado */
    printf("Ler 'nome' como INT:\n  ");
    if (row_obter_int_por_nome(&lida, "nome", &id) != 0) printf("  -> rejeitado (esperado)\n");
    printf("Gravar CHAR longo demais em 'nome':\n  ");
    if (row_definir_char_por_nome(&linha, "nome", "Gabriela") != 0) printf("  -> rejeitado (esperado)\n");

    /* 6) Percurso ordenado de todas as linhas */
    printf("Percurso da tabela 'alunos':\n");
    int n = row_percorrer(&cat, "alunos", imprime_linha_cb, NULL);
    printf("  %d linha(s) visitada(s).\n", n);
    
    remove("alunos.dat");
    return 0;
}

#endif /* ROW_SEM_MAIN */

#endif /* ROW */
