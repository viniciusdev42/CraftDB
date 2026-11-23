/* ============================================================
 *              QUERY RESULT (RESULTADO DE CONSULTA)
 * ============================================================
 *
 * Interface unica para CONSUMIR o resultado de um SELECT, sem que quem
 * consome precise conhecer arvore B+, cache, paginas ou o formato fisico
 * de 8 bytes do registro.
 *
 * O QueryResult guarda duas coisas:
 *
 *   1) METADADOS das colunas projetadas (nome, tipo, tamanho e a posicao
 *      da coluna na tabela de origem), NA ORDEM EM QUE FORAM SOLICITADAS.
 *      Ex.: SELECT nome, id FROM alunos  ->  colunas[0] = nome, colunas[1] = id
 *      (mesmo que, na tabela, 'id' venha antes de 'nome').
 *
 *   2) LINHAS MATERIALIZADAS em memoria. Cada linha traz apenas os valores
 *      das colunas projetadas, na mesma ordem dos metadados: o valor na
 *      posicao i da linha corresponde sempre a colunas[i].
 *
 * Como as linhas sao disponibilizadas (duas formas equivalentes):
 *   - ACESSO ALEATORIO:  qr_linha(qr, i)      -> i-esima linha (base 0)
 *   - CURSOR SEQUENCIAL: qr_proxima(qr)       -> proxima linha, NULL no fim
 *                        qr_reiniciar_cursor(qr) volta ao inicio
 *
 * Acesso aos valores (como em Row): por POSICAO ou por NOME da coluna
 * (nomes nao diferenciam maiusculas/minusculas, regra do Catalogo). O tipo
 * do catalogo e' preservado: ler uma coluna INT como CHAR (ou vice-versa)
 * e' rejeitado com erro.
 *
 * Ciclo de vida:   qr_executar() (ou qr_iniciar() + qr_adicionar_row())
 *                  -> consumo -> qr_liberar()
 *
 * Convencao de retorno: 0 = sucesso, -1 = erro, com mensagem em stderr
 * (funcoes que devolvem ponteiro devolvem NULL em caso de erro).
 */

#ifndef QUERY_RESULT
#define QUERY_RESULT

#define main ROW_SEM_MAIN
#include "Row.c"
#undef main
/* ------------------------------------------------------------------ */
/* Estruturas                                                          */
/* ------------------------------------------------------------------ */

/* Metadado de uma coluna projetada. */
typedef struct {
    char      nome[MAX_LEXEMA];  /* nome da coluna (grafia do catalogo)        */
    TipoToken tipo;              /* TOKEN_INT ou TOKEN_CHAR                    */
    int       tamanho;           /* bytes da coluna (limite do CHAR)           */
    int       posicao_origem;    /* posicao da coluna na tabela (base 0)       */
} ColunaResultado;

/* Valor de uma celula. Somente o membro correspondente ao tipo da coluna
   (informado pelos metadados) e' significativo. */
typedef struct {
    int32_t inteiro;             /* se a coluna for TOKEN_INT                  */
    char    texto[TAM_CHAR + 1]; /* se a coluna for TOKEN_CHAR                 */
} ValorResultado;

/* Uma linha do resultado: um valor por coluna projetada, na ordem dos
   metadados (valores[i] <-> colunas[i]). */
typedef struct {
    ValorResultado valores[MAX_COLUNAS];
} LinhaResultado;

typedef struct {
    char             tabela[MAX_LEXEMA];        /* tabela de origem            */
    ColunaResultado  colunas[MAX_COLUNAS];      /* colunas na ordem pedida     */
    int              numero_colunas;
    LinhaResultado  *linhas;                    /* vetor dinamico              */
    int              numero_linhas;
    int              capacidade;                /* linhas alocadas             */
    int              cursor;                    /* proxima linha de qr_proxima */
} QueryResult;

/* ------------------------------------------------------------------ */
/* Criacao e liberacao                                                 */
/* ------------------------------------------------------------------ */

/*
 qr_iniciar(qr, tabela, nomes, n)
 Cria um resultado vazio projetando as colunas 'nomes[0..n-1]' da tabela, NESSA
 ORDEM. Se 'nomes' for NULL ou n == 0, projeta todas as colunas (equivale a
 SELECT *), na ordem do catalogo. Uma mesma coluna pode ser pedida mais de uma
 vez; a busca por nome devolve entao a primeira ocorrencia.
 Retorna 0 em caso de sucesso, ou -1 (nenhuma memoria fica alocada) se algum
 argumento for invalido ou alguma coluna nao existir na tabela.
*/
int qr_iniciar(QueryResult *qr, const TabelaCat *tabela,
               const char *const *nomes, int n) {
    if (qr == NULL || tabela == NULL) {
        fprintf(stderr, "Erro: qr_iniciar recebeu argumento NULL\n");
        return -1;
    }

    memset(qr, 0, sizeof(*qr));
    strcpy(qr->tabela, tabela->nome);

    int todas = (nomes == NULL || n == 0);
    int total = todas ? tabela->numero_colunas : n;

    if (total < 0 || total > MAX_COLUNAS) {
        fprintf(stderr, "Erro: projecao com %d coluna(s); o maximo e %d\n", total, MAX_COLUNAS);
        return -1;
    }

    for (int i = 0; i < total; i++) {
        int origem = i;
        if (!todas) {
            if (nomes[i] == NULL) {
                fprintf(stderr, "Erro: nome de coluna NULL na projecao (posicao %d)\n", i);
                memset(qr, 0, sizeof(*qr));
                return -1;
            }
            origem = tabela_buscar_coluna(tabela, nomes[i]);
            if (origem < 0) {
                fprintf(stderr, "Erro: coluna '%s' nao existe na tabela '%s'\n",
                        nomes[i], tabela->nome);
                memset(qr, 0, sizeof(*qr));
                return -1;
            }
        }

        const ColunaCat *c = &tabela->colunas[origem];
        strcpy(qr->colunas[i].nome, c->nome);
        qr->colunas[i].tipo           = c->tipo;
        qr->colunas[i].tamanho        = c->tamanho;
        qr->colunas[i].posicao_origem = origem;
    }
    qr->numero_colunas = total;
    return 0;
}

/* Libera a memoria das linhas e zera o resultado. Pode ser chamada com um
   resultado ja liberado ou nunca iniciado (zerado). */
void qr_liberar(QueryResult *qr) {
    if (qr == NULL) {
        return;
    }
    free(qr->linhas);
    memset(qr, 0, sizeof(*qr));
}

/* Garante espaco para mais uma linha (crescimento por dobra). */
static int qr_garantir_capacidade(QueryResult *qr) {
    if (qr->numero_linhas < qr->capacidade) {
        return 0;
    }
    int nova = (qr->capacidade == 0) ? 8 : qr->capacidade * 2;
    LinhaResultado *p = realloc(qr->linhas, (size_t)nova * sizeof(*p));
    if (p == NULL) {
        fprintf(stderr, "Erro: memoria insuficiente para o resultado da tabela '%s'\n", qr->tabela);
        return -1;
    }
    qr->linhas    = p;
    qr->capacidade = nova;
    return 0;
}

/*
 qr_adicionar_row(qr, row)
 Projeta a Row (esquema da tabela inteira) sobre as colunas do resultado e
 acrescenta a linha ao fim. Confere tabela e tipo de cada coluna projetada.
 ATENCAO: ponteiros obtidos antes por qr_linha()/qr_proxima() podem ficar
 invalidos apos uma adicao (o vetor pode ser realocado).
 Retorna 0 em caso de sucesso, ou -1 em caso de erro.
*/
int qr_adicionar_row(QueryResult *qr, const Row *row) {
    if (qr == NULL || row == NULL) {
        fprintf(stderr, "Erro: qr_adicionar_row recebeu argumento NULL\n");
        return -1;
    }
    if (!contexto_igual(qr->tabela, row->tabela)) {
        fprintf(stderr, "Erro: a Row da tabela '%s' nao pertence ao resultado da tabela '%s'\n",
                row->tabela, qr->tabela);
        return -1;
    }

    /* valida tudo antes de alocar/gravar, para nao deixar linha pela metade */
    for (int i = 0; i < qr->numero_colunas; i++) {
        int origem = qr->colunas[i].posicao_origem;
        if (origem >= row->numero_campos || row->campos[origem].tipo != qr->colunas[i].tipo) {
            fprintf(stderr, "Erro: a Row nao confere com a coluna '%s' do resultado\n",
                    qr->colunas[i].nome);
            return -1;
        }
    }
    if (qr_garantir_capacidade(qr) != 0) {
        return -1;
    }

    LinhaResultado *linha = &qr->linhas[qr->numero_linhas];
    memset(linha, 0, sizeof(*linha));
    for (int i = 0; i < qr->numero_colunas; i++) {
        const CampoRow *campo = &row->campos[qr->colunas[i].posicao_origem];
        if (campo->tipo == TOKEN_INT) {
            linha->valores[i].inteiro = campo->inteiro;
        } else {
            strcpy(linha->valores[i].texto, campo->texto);
        }
    }
    qr->numero_linhas++;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Execucao: tabela (B+ / cache / pager) -> QueryResult                */
/* ------------------------------------------------------------------ */

/* Contexto que adapta o callback de Row (row_percorrer) ao QueryResult. */
typedef struct {
    QueryResult *qr;
    int        (*filtro)(const Row *, void *);
    void        *arg;
    int          erro;
} ContextoQR;

static int adaptador_qr(const Row *row, void *arg) {
    ContextoQR *ctx = (ContextoQR *)arg;
    if (ctx->filtro != NULL && !ctx->filtro(row, ctx->arg)) {
        return 0;   /* linha descartada pelo filtro; segue o percurso */
    }
    if (qr_adicionar_row(ctx->qr, row) != 0) {
        ctx->erro = 1;
        return 1;   /* interrompe o percurso */
    }
    return 0;
}

/*
 qr_executar(cat, nome_tabela, nomes, n, filtro, arg, qr)
 Le a tabela em ordem crescente da chave e monta o resultado com as colunas
 'nomes[0..n-1]' (NULL/0 = todas, veja qr_iniciar). Se 'filtro' nao for NULL,
 somente as linhas para as quais filtro(row, arg) devolver != 0 entram.
 Em caso de erro nao resta memoria alocada em *qr.
 Retorna o numero de linhas do resultado (>= 0), ou -1 em caso de erro.
*/
int qr_executar(Catalogo *cat, const char *nome_tabela,
                const char *const *nomes, int n,
                int (*filtro)(const Row *, void *), void *arg,
                QueryResult *qr) {
    if (cat == NULL || nome_tabela == NULL || qr == NULL) {
        fprintf(stderr, "Erro: qr_executar recebeu argumento NULL\n");
        return -1;
    }

    const TabelaCat *tabela = catalogo_buscar_tabela(cat, nome_tabela);
    if (tabela == NULL) {
        fprintf(stderr, "Erro: tabela '%s' nao existe no catalogo\n", nome_tabela);
        return -1;
    }
    if (qr_iniciar(qr, tabela, nomes, n) != 0) {
        return -1;
    }

    ContextoQR ctx = { qr, filtro, arg, 0 };
    if (row_percorrer(cat, nome_tabela, adaptador_qr, &ctx) < 0 || ctx.erro) {
        qr_liberar(qr);
        return -1;
    }
    return qr->numero_linhas;
}

/* ------------------------------------------------------------------ */
/* Metadados                                                           */
/* ------------------------------------------------------------------ */

int qr_numero_colunas(const QueryResult *qr) {
    return (qr == NULL) ? -1 : qr->numero_colunas;
}

/* Metadado da coluna na posicao pos (base 0), ou NULL se for invalida. */
const ColunaResultado *qr_coluna(const QueryResult *qr, int pos) {
    if (qr == NULL) {
        fprintf(stderr, "Erro: QueryResult NULL\n");
        return NULL;
    }
    if (pos < 0 || pos >= qr->numero_colunas) {
        fprintf(stderr, "Erro: posicao de coluna invalida (%d) no resultado de '%s' (%d coluna(s))\n",
                pos, qr->tabela, qr->numero_colunas);
        return NULL;
    }
    return &qr->colunas[pos];
}

/* Posicao (base 0) da coluna 'nome' NO RESULTADO, ou -1 se nao foi projetada. */
int qr_posicao_da_coluna(const QueryResult *qr, const char *nome) {
    if (qr == NULL || nome == NULL) {
        return -1;
    }
    for (int i = 0; i < qr->numero_colunas; i++) {
        if (contexto_igual(qr->colunas[i].nome, nome)) {
            return i;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Linhas: acesso aleatorio e cursor                                   */
/* ------------------------------------------------------------------ */

int qr_numero_linhas(const QueryResult *qr) {
    return (qr == NULL) ? -1 : qr->numero_linhas;
}

/* i-esima linha (base 0), ou NULL se o indice for invalido. */
const LinhaResultado *qr_linha(const QueryResult *qr, int i) {
    if (qr == NULL) {
        fprintf(stderr, "Erro: QueryResult NULL\n");
        return NULL;
    }
    if (i < 0 || i >= qr->numero_linhas) {
        fprintf(stderr, "Erro: indice de linha invalido (%d); o resultado tem %d linha(s)\n",
                i, qr->numero_linhas);
        return NULL;
    }
    return &qr->linhas[i];
}

/* Proxima linha do cursor, ou NULL quando acabaram (fim normal, sem mensagem). */
const LinhaResultado *qr_proxima(QueryResult *qr) {
    if (qr == NULL || qr->cursor >= qr->numero_linhas) {
        return NULL;
    }
    return &qr->linhas[qr->cursor++];
}

/* Volta o cursor para antes da primeira linha. */
void qr_reiniciar_cursor(QueryResult *qr) {
    if (qr != NULL) {
        qr->cursor = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Valores (por posicao e por nome)                                    */
/* ------------------------------------------------------------------ */

/* Valida linha/posicao/tipo e devolve a celula, ou NULL (com mensagem). */
static const ValorResultado *qr_celula(const QueryResult *qr, const LinhaResultado *linha,
                                       int pos, TipoToken esperado) {
    const ColunaResultado *coluna = qr_coluna(qr, pos);
    if (coluna == NULL) {
        return NULL;
    }
    if (linha == NULL) {
        fprintf(stderr, "Erro: linha NULL\n");
        return NULL;
    }
    if (coluna->tipo != esperado) {
        fprintf(stderr, "Erro: a coluna '%s' do resultado e %s, nao %s\n", coluna->nome,
                coluna->tipo == TOKEN_INT ? "INT" : "CHAR",
                esperado == TOKEN_INT ? "INT" : "CHAR");
        return NULL;
    }
    return &linha->valores[pos];
}

/* Le o valor INT da coluna na posicao pos. Erro se a coluna for CHAR. */
int qr_obter_int(const QueryResult *qr, const LinhaResultado *linha, int pos, int32_t *saida) {
    const ValorResultado *v = qr_celula(qr, linha, pos, TOKEN_INT);
    if (v == NULL || saida == NULL) {
        return -1;
    }
    *saida = v->inteiro;
    return 0;
}

/* Texto da coluna CHAR na posicao pos (ponteiro interno da linha, valido
   ate qr_liberar), ou NULL se invalida ou se a coluna for INT. */
const char *qr_obter_char(const QueryResult *qr, const LinhaResultado *linha, int pos) {
    const ValorResultado *v = qr_celula(qr, linha, pos, TOKEN_CHAR);
    return (v == NULL) ? NULL : v->texto;
}

/* Resolve nome -> posicao no resultado, emitindo erro se nao foi projetada. */
static int qr_resolve_nome(const QueryResult *qr, const char *nome) {
    int pos = qr_posicao_da_coluna(qr, nome);
    if (pos < 0) {
        fprintf(stderr, "Erro: coluna '%s' nao faz parte do resultado de '%s'\n",
                nome != NULL ? nome : "(null)", qr != NULL ? qr->tabela : "?");
    }
    return pos;
}

int qr_obter_int_por_nome(const QueryResult *qr, const LinhaResultado *linha,
                          const char *coluna, int32_t *saida) {
    int pos = qr_resolve_nome(qr, coluna);
    return (pos < 0) ? -1 : qr_obter_int(qr, linha, pos, saida);
}

const char *qr_obter_char_por_nome(const QueryResult *qr, const LinhaResultado *linha,
                                   const char *coluna) {
    int pos = qr_resolve_nome(qr, coluna);
    return (pos < 0) ? NULL : qr_obter_char(qr, linha, pos);
}

/* ------------------------------------------------------------------ */
/* Impressao                                                           */
/* ------------------------------------------------------------------ */

/* Imprime cabecalho (nomes das colunas na ordem pedida), as linhas e o total. */
void qr_imprimir(const QueryResult *qr) {
    if (qr == NULL) {
        return;
    }
    printf("  ");
    for (int i = 0; i < qr->numero_colunas; i++) {
        printf("%s%s", i ? " | " : "", qr->colunas[i].nome);
    }
    printf("\n");

    for (int l = 0; l < qr->numero_linhas; l++) {
        printf("  ");
        for (int i = 0; i < qr->numero_colunas; i++) {
            if (i) printf(" | ");
            if (qr->colunas[i].tipo == TOKEN_INT) printf("%d", qr->linhas[l].valores[i].inteiro);
            else                                  printf("'%s'", qr->linhas[l].valores[i].texto);
        }
        printf("\n");
    }
    if (qr->numero_linhas == 0) {
        printf("  (nenhum registro encontrado)\n");
    }
    printf("  %d registro(s).\n", qr->numero_linhas);
}

/* ------------------------------------------------------------------ */
/* Demonstracao                                                        */
/* ------------------------------------------------------------------ */

/* Filtro de exemplo: mantem as linhas com id >= *arg. */
static int filtro_id_minimo(const Row *row, void *arg) {
    int32_t id = 0;
    if (row_obter_int(row, 0, &id) != 0) {
        return 0;
    }
    return id >= *(const int32_t *)arg;
}
#ifdef QUERY_RESULT_SEM_MAIN
int main(void) {
    static Catalogo cat;
    Coluna colunas[2];
    Row linha;
    QueryResult qr;

    remove("alunos.dat");   /* parte de um arquivo de dados limpo */
    catalogo_iniciar(&cat);

    /* 1) Catalogo + dados: alunos (id INT, nome CHAR) */
    strcpy(colunas[0].nome, "id");   colunas[0].tipo = TOKEN_INT;
    strcpy(colunas[1].nome, "nome"); colunas[1].tipo = TOKEN_CHAR;
    if (!catalogo_adicionar_tabela(&cat, "alunos", colunas, 2)) {
        fprintf(stderr, "Erro no catalogo: %s\n", cat.mensagem);
        return 1;
    }
    const TabelaCat *tabela = catalogo_buscar_tabela(&cat, "alunos");

    const char *nomes[] = { "Ana", "Bia", "Caio", "Davi", "Eva" };
    for (int i = 0; i < 5; i++) {
        row_iniciar(&linha, tabela);
        row_definir_int(&linha, 0, 10 * (i + 1));
        row_definir_char(&linha, 1, nomes[i]);
        if (row_inserir(&cat, &linha) != INSERE_OK) {
            fprintf(stderr, "Falha ao inserir a linha %d\n", i);
            return 1;
        }
    }

    /* 2) A ORDEM DAS COLUNAS PEDIDAS E' PRESERVADA (nome antes de id) */
    const char *projecao[] = { "nome", "ID" };   /* 'ID' casa sem diferenciar caixa */
    int total = qr_executar(&cat, "alunos", projecao, 2, NULL, NULL, &qr);
    if (total < 0) {
        return 1;
    }
    printf("SELECT nome, id FROM alunos:\n");
    qr_imprimir(&qr);

    /* 3) Metadados das colunas projetadas */
    printf("\nMetadados (%d coluna(s)):\n", qr_numero_colunas(&qr));
    for (int i = 0; i < qr_numero_colunas(&qr); i++) {
        const ColunaResultado *c = qr_coluna(&qr, i);
        printf("  [%d] %-5s %-5s %d byte(s)  (coluna %d da tabela)\n", i, c->nome,
               c->tipo == TOKEN_INT ? "INT" : "CHAR", c->tamanho, c->posicao_origem);
    }

    /* 4) Cursor sequencial, com acesso por nome e por posicao */
    printf("\nCursor:\n");
    const LinhaResultado *l;
    while ((l = qr_proxima(&qr)) != NULL) {
        int32_t id = 0;
        const char *nome = qr_obter_char(&qr, l, 0);                 /* por posicao */
        qr_obter_int_por_nome(&qr, l, "id", &id);                    /* por nome    */
        printf("  %s tem id %d\n", nome, id);
    }
    qr_reiniciar_cursor(&qr);
    printf("Cursor reiniciado -> primeira linha: %s\n", qr_obter_char(&qr, qr_proxima(&qr), 0));

    /* 5) Acesso aleatorio e validacao de tipo / indices */
    int32_t v = 0;
    printf("\nLinha 3 (acesso aleatorio): %s\n", qr_obter_char_por_nome(&qr, qr_linha(&qr, 3), "nome"));
    printf("Ler 'nome' como INT:\n  ");
    if (qr_obter_int_por_nome(&qr, qr_linha(&qr, 0), "nome", &v) != 0) printf("  -> rejeitado (esperado)\n");
    printf("Linha 99:\n  ");
    if (qr_linha(&qr, 99) == NULL) printf("  -> rejeitada (esperado)\n");
    printf("Coluna fora da projecao:\n  ");
    if (qr_posicao_da_coluna(&qr, "idade") < 0) printf("  -> nao projetada (esperado)\n");
    qr_liberar(&qr);

    /* 6) SELECT * com filtro */
    int32_t minimo = 30;
    total = qr_executar(&cat, "alunos", NULL, 0, filtro_id_minimo, &minimo, &qr);
    printf("\nSELECT * FROM alunos WHERE id >= 30 (%d linha(s)):\n", total);
    qr_imprimir(&qr);
    qr_liberar(&qr);

    /* 7) Projecao invalida nao deixa resultado nem memoria alocada */
    const char *invalida[] = { "id", "idade" };
    printf("\nSELECT id, idade FROM alunos:\n  ");
    if (qr_executar(&cat, "alunos", invalida, 2, NULL, NULL, &qr) < 0) printf("  -> rejeitado (esperado)\n");

    remove("alunos.dat");
    return 0;
}
#endif /*QUERY_RESULT_SEM_MAIN*/
#endif /* QUERY_RESULT */
