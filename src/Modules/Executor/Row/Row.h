#ifndef ROW
#define ROW

#include <stddef.h>
#include <stdint.h>

#ifndef MAX_COLUNAS
#define MAX_COLUNAS 16      /* ajuste para o MAX_COLUNAS do parser    */
#endif

#ifndef MAX_NOME
#define MAX_NOME 64         /* ajuste para o MAX_LEXEMA do parser     */
#endif

#ifndef MAX_TEXTO
#define MAX_TEXTO 32        /* maximo de caracteres de um CHAR        */
#endif

#define TAM_INT 4           /* mesmo valor de TAM_INT do catalogo     */

/* Tipos de coluna (independentes de TOKEN_INT / TOKEN_CHAR do parser). */
typedef enum {
    ROW_TIPO_INT = 0,
    ROW_TIPO_CHAR
} TipoRow;

/* Codigos de retorno: 0 = sucesso, valores negativos = erro. */
typedef enum {
    ROW_OK                      =  0,
    ROW_ERRO_ARGUMENTO          = -1,  /* ponteiro nulo / argumento invalido   */
    ROW_ERRO_COLUNA_INEXISTENTE = -2,  /* posicao ou nome nao existe           */
    ROW_ERRO_TIPO               = -3,  /* tipo do valor difere do da coluna    */
    ROW_ERRO_TAMANHO            = -4,  /* texto maior que a capacidade da col. */
    ROW_ERRO_NULL_PROIBIDO      = -5,  /* NULL em coluna que nao aceita NULL   */
    ROW_ERRO_VALOR_NULL         = -6,  /* leitura de valor que esta NULL       */
    ROW_ERRO_ESQUEMA_CHEIO      = -7,  /* MAX_COLUNAS atingido             */
    ROW_ERRO_COLUNA_DUPLICADA   = -8,
    ROW_ERRO_NOME_INVALIDO      = -9,
    ROW_ERRO_BUFFER_PEQUENO     = -10
}StatusRow;

/* ------------------------------------------------------------------ */
/* Esquema                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    char    nome[MAX_NOME];
    TipoRow tipo;
    int     tamanho;      /* INT: TAM_INT; CHAR: maximo de caracteres */
    int     aceita_null;  /* 0 = NOT NULL, 1 = aceita NULL                */
} ColunaRow;

typedef struct {
    ColunaRow colunas[MAX_COLUNAS];
    int       numero_colunas;
} EsquemaRow;

void iniciar_esquema(EsquemaRow *esquema);

/* Para colunas INT o parametro 'tamanho' e ignorado (fixo em TAM_INT). */
StatusRow adicionar_coluna(EsquemaRow *esquema, const char *nome,
                                        TipoRow tipo, int tamanho,
                                        int aceita_null);

/* Retorna a posicao da coluna ou -1 se nao existir. */
int buscar_coluna(const EsquemaRow *esquema, const char *nome);

/* Retorna a coluna na posicao 'posicao' ou NULL se for invalida. */
const ColunaRow *coluna_esquema(const EsquemaRow *esquema, int posicao);

/* ------------------------------------------------------------------ */
/* Row                                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    int nulo;                          /* 1 = valor NULL                 */
    union {
        int32_t inteiro;
        char    texto[MAX_TEXTO + 1];
    } dado;
} ValorRow;

typedef struct {
    const EsquemaRow *esquema;         /* nao pertence a Row             */
    ValorRow valores[MAX_COLUNAS];
} Row;

/* Cria uma linha com todas as colunas NULL. */
StatusRow iniciar_row(Row *linha, const EsquemaRow *esquema);

/* Metadados */
int          tipo_coluna(const Row *linha);             /* -1 se invalida */
const char  *nome_coluna(const Row *linha, int posicao);   /* NULL se invalida */
int          tipo_coluna(const Row *linha, int posicao);   /* TipoRow ou <0   */
int          buscar_coluna(const Row *linha, const char *nome);

/* NULL: retorna 1 (NULL), 0 (nao NULL) ou codigo de erro (< 0). */
int  null_por_posicao(const Row *linha, int posicao);
int  null_por_nome(const Row *linha, const char *nome);

/* Escrita por posicao */
StatusRow definir_inteiro(Row *linha, int posicao, int32_t valor);
StatusRow definir_texto(Row *linha, int posicao, const char *texto);
StatusRow definir_null(Row *linha, int posicao);

/* Escrita por nome */
StatusRow definir_inteiro_por_nome(Row *linha, const char *nome, int32_t valor);
StatusRow definir_texto_por_nome(Row *linha, const char *nome, const char *texto);
StatusRow definir_null_por_nome(Row *linha, const char *nome);

/* Leitura por posicao. Em valor NULL retornam ROW_ERRO_VALOR_NULL.
 * O ponteiro devolvido por obter_texto aponta para dentro da Row e
 * vale enquanto a Row existir e o valor nao for alterado. */
StatusRow obter_inteiro(const Row *linha, int posicao, int32_t *saida);
StatusRow obter_texto(const Row *linha, int posicao, const char **saida);

/* Leitura por nome */
StatusRow obter_inteiro_por_nome(const Row *linha, const char *nome, int32_t *saida);
StatusRow obter_texto_por_nome(const Row *linha, const char *nome, const char **saida);

/* Verifica as restricoes NOT NULL. Em caso de falha retorna
 * ROW_ERRO_NULL_PROIBIDO e, se 'coluna_invalida' != NULL, a posicao da
 * primeira coluna violada. */
StatusRow validar_coluna(const Row *linha, int *coluna_invalida);

/* Escreve o valor da coluna como texto legivel: NULL -> "NULL",
 * INT -> "42", CHAR -> "'abc'". */
StatusRow formatar_valor(const Row *linha, int posicao, char *saida, size_t capacidade);

/* Mensagem descritiva para um codigo de retorno. */
const char *mensagem_status(StatusRow status);

#endif /* ROW */
