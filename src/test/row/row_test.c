#define ROW_MAIN
#include "row.c"

#include <assert.h>

static TabelaCat criar_tabela_teste(void) {
    Catalogo catalogo;
    Coluna colunas[2] = {0};

    catalogo_iniciar(&catalogo);

    strcpy(colunas[0].nome, "id");
    colunas[0].tipo = TOKEN_INT;

    strcpy(colunas[1].nome, "nome");
    colunas[1].tipo = TOKEN_CHAR;

    assert(catalogo_adicionar_tabela(&catalogo, "alunos", colunas, 2) == 1);

    const TabelaCat *tabela = catalogo_buscar_tabela(&catalogo, "alunos");
    assert(tabela != NULL);
    return *tabela;
}

static void testar_criacao_e_consulta_de_esquema(void) {
    TabelaCat tabela = criar_tabela_teste();
    Row row = {0};
    TipoToken tipo = TOKEN_ERRO;

    assert(row_iniciar(&row, &tabela) == 0);
    assert(strcmp(row.tabela, "alunos") == 0);
    assert(row.numero_campos == 2);

    assert(row_posicao_da_coluna(&row, "id") == 0);
    assert(row_posicao_da_coluna(&row, "NOME") == 1);
    assert(row_posicao_da_coluna(&row, "inexistente") == -1);

    assert(row_tipo_da_coluna(&row, 0, &tipo) == 0);
    assert(tipo == TOKEN_INT);
    assert(row_tipo_da_coluna(&row, 1, &tipo) == 0);
    assert(tipo == TOKEN_CHAR);

    assert(row_tipo_da_coluna(&row, 2, &tipo) == -1);
    assert(row_tipo_da_coluna(&row, 0, NULL) == -1);
    assert(row_iniciar(NULL, &tabela) == -1);
    assert(row_iniciar(&row, NULL) == -1);
}

static void testar_leitura_e_escrita(void) {
    TabelaCat tabela = criar_tabela_teste();
    Row row;
    int32_t id = -1;

    assert(row_iniciar(&row, &tabela) == 0);

    /* Escrita e leitura por nome, sem diferenciar maiusculas/minusculas. */
    assert(row_definir_int_por_nome(&row, "ID", 42) == 0);
    assert(row_definir_char_por_nome(&row, "Nome", "Ana") == 0);
    assert(row_obter_int_por_nome(&row, "id", &id) == 0);
    assert(id == 42);
    assert(strcmp(row_obter_char_por_nome(&row, "NOME"), "Ana") == 0);

    /* Escrita e leitura por posicao. */
    assert(row_definir_int(&row, 0, 77) == 0);
    assert(row_definir_char(&row, 1, "Bia") == 0);
    assert(row_obter_int(&row, 0, &id) == 0);
    assert(id == 77);
    assert(strcmp(row_obter_char(&row, 1), "Bia") == 0);

    /* Tipos, posicoes, nomes e limites invalidos. */
    assert(row_definir_int(&row, 1, 10) == -1);
    assert(row_definir_char(&row, 0, "x") == -1);
    assert(row_definir_char(&row, 1, "longo") == -1);
    assert(row_definir_char(&row, 1, NULL) == -1);
    assert(row_definir_int(&row, 2, 1) == -1);
    assert(row_obter_int(&row, 1, &id) == -1);
    assert(row_obter_char(&row, 0) == NULL);
    assert(row_obter_int(&row, 0, NULL) == -1);
    assert(row_obter_int_por_nome(&row, "ausente", &id) == -1);
    assert(row_obter_char_por_nome(&row, "ausente") == NULL);
    assert(row_definir_int_por_nome(&row, "ausente", 1) == -1);
    assert(row_definir_char_por_nome(&row, "ausente", "x") == -1);
}

static void testar_conversao_row_registro(void) {
    TabelaCat tabela = criar_tabela_teste();
    Row original, reconstruida;
    Registro registro = {0};
    int32_t id = 0;

    assert(row_iniciar(&original, &tabela) == 0);
    assert(row_definir_int(&original, 0, 123) == 0);
    assert(row_definir_char(&original, 1, "Leo") == 0);

    assert(row_para_registro(&original, &registro) == 0);
    assert(registro.id == 123);

    assert(row_de_registro(&reconstruida, &tabela, &registro) == 0);
    assert(row_obter_int(&reconstruida, 0, &id) == 0);
    assert(id == 123);
    assert(strcmp(row_obter_char(&reconstruida, 1), "Leo") == 0);

    assert(row_para_registro(NULL, &registro) == -1);
    assert(row_para_registro(&original, NULL) == -1);
    assert(row_de_registro(&reconstruida, &tabela, NULL) == -1);
}

int main(void) {
    testar_criacao_e_consulta_de_esquema();
    testar_leitura_e_escrita();
    testar_conversao_row_registro();

    puts("Todos os testes de Row passaram.");
    return 0;
}
