#include "Row.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Auxiliares internos                                                 */
/* ------------------------------------------------------------------ */

static int nomes_iguais(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static int nome_valido(const char *nome)
{
    size_t tamanho;

    if (nome == NULL || !(isalpha((unsigned char)nome[0]) || nome[0] == '_'))
        return 0;
    for (size_t i = 1; nome[i]; i++)
        if (!(isalnum((unsigned char)nome[i]) || nome[i] == '_'))
            return 0;

    tamanho = strlen(nome);
    return tamanho < ROW_MAX_NOME;
}

static int linha_valida(const Row *linha)
{
    return linha != NULL && linha->esquema != NULL;
}

static int posicao_valida(const Row *linha, int posicao)
{
    return linha_valida(linha)
        && posicao >= 0
        && posicao < linha->esquema->numero_colunas
        && posicao < ROW_MAX_COLUNAS;
}

/* Resolve nome -> posicao, centralizando a validacao das variantes por nome. */
static RowStatus resolver_posicao(const Row *linha, const char *nome, int *posicao)
{
    if (!linha_valida(linha) || nome == NULL || posicao == NULL)
        return ROW_ERRO_ARGUMENTO;

    *posicao =  buscar_coluna_esquema(linha->esquema, nome);
    return (*posicao < 0) ? ROW_ERRO_COLUNA_INEXISTENTE : ROW_OK;
}

/* ------------------------------------------------------------------ */
/* Esquema                                                             */
/* ------------------------------------------------------------------ */

void iniciar_esquema(EsquemaRow *esquema)
{
    if (esquema != NULL)
        memset(esquema, 0, sizeof *esquema);
}

RowStatus adicionar_coluna(EsquemaRow *esquema, const char *nome,
                                       TipoRow tipo, int tamanho,
                                       int aceita_null)
{
    ColunaRow *coluna;

    if (esquema == NULL || nome == NULL)
        return ROW_ERRO_ARGUMENTO;
    if (esquema->numero_colunas >= ROW_MAX_COLUNAS)
        return ROW_ERRO_ESQUEMA_CHEIO;
    if (!nome_valido(nome))
        return ROW_ERRO_NOME_INVALIDO;
    if ( buscar_coluna_esquema(esquema, nome) >= 0)
        return ROW_ERRO_COLUNA_DUPLICADA;

    if (tipo == ROW_TIPO_INT) {
        tamanho = ROW_TAM_INT;
    } else if (tipo == ROW_TIPO_CHAR) {
        if (tamanho < 1 || tamanho > ROW_MAX_TEXTO)
            return ROW_ERRO_TAMANHO;
    } else {
        return ROW_ERRO_TIPO;
    }

    coluna = &esquema->colunas[esquema->numero_colunas];
    memset(coluna, 0, sizeof *coluna);
    strcpy(coluna->nome, nome);          /* tamanho ja validado */
    coluna->tipo        = tipo;
    coluna->tamanho     = tamanho;
    coluna->aceita_null = aceita_null ? 1 : 0;

    esquema->numero_colunas++;
    return ROW_OK;
}

int  buscar_coluna_esquema(const EsquemaRow *esquema, const char *nome)
{
    if (esquema == NULL || nome == NULL)
        return -1;
    for (int i = 0; i < esquema->numero_colunas; i++)
        if (nomes_iguais(esquema->colunas[i].nome, nome))
            return i;
    return -1;
}

const ColunaRow *coluna_esquema(const EsquemaRow *esquema, int posicao)
{
    if (esquema == NULL || posicao < 0 || posicao >= esquema->numero_colunas)
        return NULL;
    return &esquema->colunas[posicao];
}

/* ------------------------------------------------------------------ */
/* Criacao e metadados                                                 */
/* ------------------------------------------------------------------ */

RowStatus iniciar_row(Row *linha, const EsquemaRow *esquema)
{
    if (linha == NULL || esquema == NULL)
        return ROW_ERRO_ARGUMENTO;
    if (esquema->numero_colunas < 0 || esquema->numero_colunas > ROW_MAX_COLUNAS)
        return ROW_ERRO_ARGUMENTO;

    memset(linha, 0, sizeof *linha);
    linha->esquema = esquema;
    for (int i = 0; i < esquema->numero_colunas; i++)
        linha->valores[i].nulo = 1;
    return ROW_OK;
}

int numero_colunas(const Row *linha)
{
    return linha_valida(linha) ? linha->esquema->numero_colunas : -1;
}

const char * nome_coluna(const Row *linha, int posicao)
{
    return posicao_valida(linha, posicao) ? linha->esquema->colunas[posicao].nome : NULL;
}

int tipo_coluna(const Row *linha, int posicao)
{
    if (!posicao_valida(linha, posicao))
        return ROW_ERRO_COLUNA_INEXISTENTE;
    return (int)linha->esquema->colunas[posicao].tipo;
}

int buscar_coluna_linha(const Row *linha, const char *nome)
{
    return linha_valida(linha) ?  buscar_coluna_linha(linha->esquema, nome) : -1;
}

/* ------------------------------------------------------------------ */
/* NULL                                                                */
/* ------------------------------------------------------------------ */

int null_por_posicao(const Row *linha, int posicao)
{
    if (!linha_valida(linha))
        return ROW_ERRO_ARGUMENTO;
    if (!posicao_valida(linha, posicao))
        return ROW_ERRO_COLUNA_INEXISTENTE;
    return linha->valores[posicao].nulo ? 1 : 0;
}

int null_por_nome(const Row *linha, const char *nome)
{
    int posicao;
    RowStatus st = resolver_posicao(linha, nome, &posicao);
    return (st != ROW_OK) ? (int)st : null_por_posicao(linha, posicao);
}

/* ------------------------------------------------------------------ */
/* Escrita                                                             */
/* ------------------------------------------------------------------ */

RowStatus definir_inteiro(Row *linha, int posicao, int32_t valor)
{
    if (!linha_valida(linha))
        return ROW_ERRO_ARGUMENTO;
    if (!posicao_valida(linha, posicao))
        return ROW_ERRO_COLUNA_INEXISTENTE;
    if (linha->esquema->colunas[posicao].tipo != ROW_TIPO_INT)
        return ROW_ERRO_TIPO;

    memset(&linha->valores[posicao], 0, sizeof linha->valores[posicao]);
    linha->valores[posicao].dado.inteiro = valor;
    return ROW_OK;
}

RowStatus definir_texto(Row *linha, int posicao, const char *texto)
{
    const ColunaRow *coluna;
    size_t tamanho;

    if (!linha_valida(linha) || texto == NULL)
        return ROW_ERRO_ARGUMENTO;
    if (!posicao_valida(linha, posicao))
        return ROW_ERRO_COLUNA_INEXISTENTE;

    coluna = &linha->esquema->colunas[posicao];
    if (coluna->tipo != ROW_TIPO_CHAR)
        return ROW_ERRO_TIPO;

    tamanho = strlen(texto);
    if (tamanho > (size_t)coluna->tamanho || tamanho > ROW_MAX_TEXTO)
        return ROW_ERRO_TAMANHO;

    memset(&linha->valores[posicao], 0, sizeof linha->valores[posicao]);
    memcpy(linha->valores[posicao].dado.texto, texto, tamanho + 1);
    return ROW_OK;
}

RowStatus definir_null(Row *linha, int posicao)
{
    if (!linha_valida(linha))
        return ROW_ERRO_ARGUMENTO;
    if (!posicao_valida(linha, posicao))
        return ROW_ERRO_COLUNA_INEXISTENTE;
    if (!linha->esquema->colunas[posicao].aceita_null)
        return ROW_ERRO_NULL_PROIBIDO;

    memset(&linha->valores[posicao], 0, sizeof linha->valores[posicao]);
    linha->valores[posicao].nulo = 1;
    return ROW_OK;
}

RowStatus definir_inteiro_por_nome(Row *linha, const char *nome, int32_t valor)
{
    int posicao;
    RowStatus st = resolver_posicao(linha, nome, &posicao);
    return (st != ROW_OK) ? st : definir_inteiro(linha, posicao, valor);
}

RowStatus definir_texto_por_nome(Row *linha, const char *nome, const char *texto)
{
    int posicao;
    RowStatus st = resolver_posicao(linha, nome, &posicao);
    return (st != ROW_OK) ? st : definir_texto(linha, posicao, texto);
}

RowStatus definir_null_por_nome(Row *linha, const char *nome)
{
    int posicao;
    RowStatus st = resolver_posicao(linha, nome, &posicao);
    return (st != ROW_OK) ? st : definir_null(linha, posicao);
}

/* ------------------------------------------------------------------ */
/* Leitura                                                             */
/* ------------------------------------------------------------------ */

RowStatus obter_inteiro(const Row *linha, int posicao, int32_t *saida)
{
    if (!linha_valida(linha) || saida == NULL)
        return ROW_ERRO_ARGUMENTO;
    if (!posicao_valida(linha, posicao))
        return ROW_ERRO_COLUNA_INEXISTENTE;
    if (linha->esquema->colunas[posicao].tipo != ROW_TIPO_INT)
        return ROW_ERRO_TIPO;
    if (linha->valores[posicao].nulo)
        return ROW_ERRO_VALOR_NULL;

    *saida = linha->valores[posicao].dado.inteiro;
    return ROW_OK;
}

RowStatus obter_texto(const Row *linha, int posicao, const char **saida)
{
    if (!linha_valida(linha) || saida == NULL)
        return ROW_ERRO_ARGUMENTO;
    if (!posicao_valida(linha, posicao))
        return ROW_ERRO_COLUNA_INEXISTENTE;
    if (linha->esquema->colunas[posicao].tipo != ROW_TIPO_CHAR)
        return ROW_ERRO_TIPO;
    if (linha->valores[posicao].nulo)
        return ROW_ERRO_VALOR_NULL;

    *saida = linha->valores[posicao].dado.texto;
    return ROW_OK;
}

RowStatus obter_inteiro_por_nome(const Row *linha, const char *nome, int32_t *saida)
{
    int posicao;
    RowStatus st = resolver_posicao(linha, nome, &posicao);
    return (st != ROW_OK) ? st : obter_inteiro(linha, posicao, saida);
}

RowStatus obter_texto_por_nome(const Row *linha, const char *nome, const char **saida)
{
    int posicao;
    RowStatus st = resolver_posicao(linha, nome, &posicao);
    return (st != ROW_OK) ? st : obter_texto(linha, posicao, saida);
}

/* ------------------------------------------------------------------ */
/* Validacao e formatacao                                              */
/* ------------------------------------------------------------------ */

RowStatus validar(const Row *linha, int *coluna_invalida)
{
    if (!linha_valida(linha))
        return ROW_ERRO_ARGUMENTO;

    for (int i = 0; i < linha->esquema->numero_colunas; i++) {
        if (linha->valores[i].nulo && !linha->esquema->colunas[i].aceita_null) {
            if (coluna_invalida != NULL)
                *coluna_invalida = i;
            return ROW_ERRO_NULL_PROIBIDO;
        }
    }
    return ROW_OK;
}

RowStatus formatar_valor(const Row *linha, int posicao, char *saida, size_t capacidade)
{
    int escritos;

    if (!linha_valida(linha) || saida == NULL || capacidade == 0)
        return ROW_ERRO_ARGUMENTO;
    if (!posicao_valida(linha, posicao))
        return ROW_ERRO_COLUNA_INEXISTENTE;

    if (linha->valores[posicao].nulo)
        escritos = snprintf(saida, capacidade, "NULL");
    else if (linha->esquema->colunas[posicao].tipo == ROW_TIPO_INT)
        escritos = snprintf(saida, capacidade, "%d", (int)linha->valores[posicao].dado.inteiro);
    else
        escritos = snprintf(saida, capacidade, "'%s'", linha->valores[posicao].dado.texto);

    return (escritos < 0 || (size_t)escritos >= capacidade) ? ROW_ERRO_BUFFER_PEQUENO : ROW_OK;
}

const char *mensagem_status(RowStatus status)
{
    switch (status) {
        case ROW_OK:                      return "sucesso";
        case ROW_ERRO_ARGUMENTO:          return "argumento invalido";
        case ROW_ERRO_COLUNA_INEXISTENTE: return "coluna inexistente";
        case ROW_ERRO_TIPO:               return "tipo incompativel com a coluna";
        case ROW_ERRO_TAMANHO:            return "valor excede a capacidade da coluna";
        case ROW_ERRO_NULL_PROIBIDO:      return "coluna nao aceita NULL";
        case ROW_ERRO_VALOR_NULL:         return "valor da coluna e NULL";
        case ROW_ERRO_ESQUEMA_CHEIO:      return "numero maximo de colunas excedido";
        case ROW_ERRO_COLUNA_DUPLICADA:   return "coluna duplicada";
        case ROW_ERRO_NOME_INVALIDO:      return "nome de coluna invalido";
        case ROW_ERRO_BUFFER_PEQUENO:     return "buffer de saida pequeno demais";
    }
    return "erro desconhecido";
}
#endif
