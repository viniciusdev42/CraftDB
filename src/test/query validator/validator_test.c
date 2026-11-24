#define  VALIDATOR_MAIN
#include "../../modules/executor/validator/query_validator.c"

#include <stdio.h>

static int testes_executados;
static int testes_falharam;

#define CHECK(condicao) do { \
    testes_executados++; \
    if (!(condicao)) { \
        fprintf(stderr, "FALHOU: %s (%s:%d)\n", #condicao, __FILE__, __LINE__); \
        testes_falharam++; \
    } \
} while (0)

static void preparar_catalogo(Catalogo *cat)
{
    Coluna colunas[2] = {0};

    catalogo_iniciar(cat);
    strcpy(colunas[0].nome, "id");
    colunas[0].tipo = TOKEN_INT;
    strcpy(colunas[1].nome, "nome");
    colunas[1].tipo = TOKEN_CHAR;

    CHECK(catalogo_adicionar_tabela(cat, "alunos", colunas, 2));
}

static void definir_numero(Valor *valor, int numero)
{
    memset(valor, 0, sizeof *valor);
    valor->tipo = TOKEN_NUMERO;
    valor->numero = numero;
}

static void definir_string(Valor *valor, const char *texto)
{
    memset(valor, 0, sizeof *valor);
    valor->tipo = TOKEN_STRING;
    strcpy(valor->str, texto);
}

static void testar_create_sem_alterar_catalogo(void)
{
    Catalogo cat;
    Validador validador;
    Comando comando = {0};
    ConsultaValidada saida;

    catalogo_iniciar(&cat);
    comando.tipo = CMD_CREATE;
    strcpy(comando.tabela, "produtos");
    comando.numero_colunas = 2;
    strcpy(comando.colunas[0].nome, "codigo");
    comando.colunas[0].tipo = TOKEN_INT;
    strcpy(comando.colunas[1].nome, "descricao");
    comando.colunas[1].tipo = TOKEN_CHAR;

    CHECK(validar_consulta(&validador, &cat, &comando, &saida));
    CHECK(saida.valida == 1);
    CHECK(saida.tipo == CMD_CREATE);
    CHECK(saida.id_tabela == 0);
    CHECK(saida.tabela == NULL);
    CHECK(saida.nova_tabela.numero_colunas == 2);
    CHECK(saida.nova_tabela.colunas[1].deslocamento == TAM_INT);
    CHECK(cat.numero_tabelas == 0); /* CREATE e validado sobre uma copia. */
}

static void testar_insert_valido_resolve_colunas(void)
{
    Catalogo cat;
    Validador validador;
    Comando comando = {0};
    ConsultaValidada saida;

    preparar_catalogo(&cat);
    comando.tipo = CMD_INSERT;
    strcpy(comando.tabela, "ALUNOS"); /* nomes nao diferenciam caixa. */
    comando.numero_valores = 2;
    definir_numero(&comando.valores[0], 42);
    definir_string(&comando.valores[1], "Ana");

    CHECK(validar_consulta(&validador, &cat, &comando, &saida));
    CHECK(saida.valida == 1);
    CHECK(saida.id_tabela == 0);
    CHECK(saida.tabela == &cat.tabelas[0]);
    CHECK(saida.numero_valores == 2);
    CHECK(saida.valores[0].id_coluna == 0);
    CHECK(saida.valores[0].tipo_coluna == TOKEN_INT);
    CHECK(saida.valores[1].id_coluna == 1);
    CHECK(saida.valores[1].deslocamento == TAM_INT);
}

static void testar_select_valido_resolve_condicoes(void)
{
    Catalogo cat;
    Validador validador;
    Comando comando = {0};
    ConsultaValidada saida;

    preparar_catalogo(&cat);
    comando.tipo = CMD_SELECT;
    strcpy(comando.tabela, "alunos");
    comando.numero_condicoes = 2;

    strcpy(comando.condicoes[0].coluna, "id");
    comando.condicoes[0].operacao = TOKEN_MAIOR_IGUAL;
    definir_numero(&comando.condicoes[0].valor, 10);

    strcpy(comando.condicoes[1].coluna, "nome");
    comando.condicoes[1].operacao = TOKEN_DIFERENTE;
    definir_string(&comando.condicoes[1].valor, "Bob");

    CHECK(validar_consulta(&validador, &cat, &comando, &saida));
    CHECK(saida.valida == 1);
    CHECK(saida.numero_condicoes == 2);
    CHECK(saida.condicoes[0].id_coluna == 0);
    CHECK(saida.condicoes[0].operacao == TOKEN_MAIOR_IGUAL);
    CHECK(saida.condicoes[1].id_coluna == 1);
    CHECK(saida.condicoes[1].tipo_coluna == TOKEN_CHAR);
}

static void testar_insert_incompativel_rejeita_e_zera_saida(void)
{
    Catalogo cat;
    Validador validador;
    Comando comando = {0};
    ConsultaValidada saida;

    preparar_catalogo(&cat);
    comando.tipo = CMD_INSERT;
    strcpy(comando.tabela, "alunos");
    comando.numero_valores = 2;
    definir_string(&comando.valores[0], "nao-e-inteiro");
    definir_string(&comando.valores[1], "Ana");
    memset(&saida, 0xA5, sizeof saida);

    CHECK(!validar_consulta(&validador, &cat, &comando, &saida));
    CHECK(saida.valida == 0);
    CHECK(saida.tabela == NULL);
    CHECK(saida.numero_valores == 0);
    CHECK(validador.mensagem[0] != '\0');
}

static void testar_select_com_coluna_inexistente_rejeita(void)
{
    Catalogo cat;
    Validador validador;
    Comando comando = {0};
    ConsultaValidada saida;

    preparar_catalogo(&cat);
    comando.tipo = CMD_SELECT;
    strcpy(comando.tabela, "alunos");
    comando.numero_condicoes = 1;
    strcpy(comando.condicoes[0].coluna, "idade");
    comando.condicoes[0].operacao = TOKEN_IGUAL;
    definir_numero(&comando.condicoes[0].valor, 18);

    CHECK(!validar_consulta(&validador, &cat, &comando, &saida));
    CHECK(saida.valida == 0);
    CHECK(saida.tabela == NULL);
    CHECK(saida.numero_condicoes == 0);
    CHECK(strstr(validador.mensagem, "idade") != NULL);
}

int main(void)
{
    testar_create_sem_alterar_catalogo();
    testar_insert_valido_resolve_colunas();
    testar_select_valido_resolve_condicoes();
    testar_insert_incompativel_rejeita_e_zera_saida();
    testar_select_com_coluna_inexistente_rejeita();

    if (testes_falharam != 0) {
        fprintf(stderr, "\n%d de %d verificacao(oes) falharam.\n",
                testes_falharam, testes_executados);
        return 1;
    }

    printf("OK: %d verificacao(oes) executada(s).\n", testes_executados);
    return 0;
}
