/*
 * Validador de Consultas (Query Validator) - analise semantica.
 *
 * Posicao no pipeline:
 *
 *   texto SQL -> Tokenizer -> Parser (AST) -> VALIDADOR -> executor
 *                                                 |
 *                                                 +-- consulta o CATALOGO
 *
 * Recebe o Comando (AST) produzido pelo Parser e:
 *
 *   1. confere a integridade estrutural da AST (contagens, strings
 *      terminadas, campos que nao pertencem ao tipo do comando);
 *   2. rejeita funcionalidades nao suportadas (tipo de comando, operador,
 *      tipo de valor ou de coluna fora do escopo);
 *   3. resolve a tabela no catalogo e devolve um identificador interno
 *      (id_tabela = indice da tabela no catalogo);
 *   4. resolve cada coluna para um identificador interno
 *      (id_coluna = indice da coluna na tabela, mais tipo, tamanho e
 *      deslocamento dentro do registro);
 *   5. valida operadores e tipos dos literais contra o tipo da coluna;
 *   6. devolve uma ConsultaValidada, que o executor pode usar SEM consultar
 *      nomes novamente.
 *
 * Regras de ouro:
 *   - O validador NUNCA altera o catalogo e NUNCA toca em disco. Registrar
 *     a tabela (CREATE) e executar continuam sendo papel do executor.
 *   - Toda consulta invalida e rejeitada aqui. Se validar_consulta devolver
 *     0, a saida fica zerada (valida == 0) e nada deve ser executado.
 *
 * Acoplamento: este arquivo inclui Catalog.c (que inclui Parser.c e este,
 * por sua vez, o Tokenizer.c) e reutiliza as mesmas regras do catalogo
 * (catalogo_adicionar_tabela, valor_compativel, tabela_buscar_coluna ...),
 * de modo que o validador e o catalogo nunca discordem.
 *
 * Uso (o main no final do arquivo faz apenas simulacao, nada e gravado):
 *   ./validador                      valida um exemplo embutido
 *   ./validador script.sql           valida os comandos do arquivo
 *   ./validador script.sql meu.cat   parte do catalogo gravado em meu.cat
 */

#include "Catalog.c"

/* ------------------------------------------------------------------ */
/* Representacao validada                                              */
/* ------------------------------------------------------------------ */

/* Valor de INSERT ja casado com a coluna de destino */
typedef struct {
    int       id_coluna;        /* id interno: indice da coluna na tabela   */
    TipoToken tipo_coluna;      /* TOKEN_INT ou TOKEN_CHAR                  */
    int       tamanho;          /* bytes ocupados no registro               */
    int       deslocamento;     /* posicao da coluna dentro do registro     */
    Valor     valor;
} ValorValidado;

/* Condicao de WHERE com a coluna resolvida */
typedef struct {
    int       id_coluna;
    TipoToken tipo_coluna;
    int       tamanho;
    int       deslocamento;
    TipoToken operacao;         /* TOKEN_IGUAL, TOKEN_MENOR, ...            */
    Valor     valor;
} CondicaoValidada;

typedef struct {
    int         valida;         /* 1 somente se a consulta foi aceita       */
    TipoComando tipo;
    int         id_tabela;      /* id interno: indice no catalogo. Em CREATE,
                                   e o indice que a nova tabela vai ocupar  */
    const TabelaCat *tabela;    /* tabela do catalogo (NULL em CREATE)      */

    TabelaCat        nova_tabela;                   /* CREATE: definicao pronta
                                                       (tamanhos e deslocamentos
                                                       ja calculados)        */
    ValorValidado    valores[MAX_COLUNAS];          /* INSERT: ordem das colunas */
    int              numero_valores;
    CondicaoValidada condicoes[MAX_CONDICOES];      /* SELECT ... WHERE         */
    int              numero_condicoes;
} ConsultaValidada;

typedef struct {
    char mensagem[MAX_MESSAGEM];    /* motivo da ultima rejeicao            */
} Validador;

/* ------------------------------------------------------------------ */
/* Utilitarios                                                         */
/* ------------------------------------------------------------------ */

/* Grava o motivo da rejeicao. Devolve sempre 0. */
static int rejeitar(Validador *validador, const char *formato, ...)
{
    va_list ap;
    va_start(ap, formato);
    vsnprintf(validador->mensagem, MAX_MESSAGEM, formato, ap);
    va_end(ap);
    return 0;
}

/* Garante que o buffer contem um '\0' (evita strcpy/strlen alem do vetor) */
static int texto_terminado(const char *texto, size_t capacidade)
{
    return memchr(texto, '\0', capacidade) != NULL;
}

/*
 * Ponto unico para "operador X pode ser aplicado a coluna do tipo Y?".
 * INT compara numericamente; CHAR compara lexicograficamente (strcmp),
 * igual ao executor do catalogo. Novos tipos exigem decisao explicita aqui.
 */
static int operador_aplicavel(TipoToken tipo_coluna, TipoToken operacao)
{
    switch (tipo_coluna) {
        case TOKEN_INT:
        case TOKEN_CHAR:
            return e_operador(operacao);
        default:
            return 0;
    }
}

/* ------------------------------------------------------------------ */
/* Etapa 1 e 2: integridade da AST e funcionalidades nao suportadas    */
/* ------------------------------------------------------------------ */

static int validar_valor_ast(Validador *validador, const Valor *valor,
                             const char *contexto)
{
    if (valor->tipo != TOKEN_NUMERO && valor->tipo != TOKEN_STRING)
        return rejeitar(validador, "%s: tipo de valor nao suportado (%s)",
                        contexto, nome_token(valor->tipo));
    if (!texto_terminado(valor->str, sizeof valor->str))
        return rejeitar(validador, "%s: literal malformado na AST", contexto);
    return 1;
}

static int validar_estrutura(Validador *validador, const Comando *comando)
{
    switch (comando->tipo) {
        case CMD_CREATE:
        case CMD_INSERT:
        case CMD_SELECT:
            break;
        default:
            return rejeitar(validador, "tipo de comando nao suportado (%d)",
                            (int)comando->tipo);
    }

    if (!texto_terminado(comando->tabela, sizeof comando->tabela))
        return rejeitar(validador, "nome de tabela malformado na AST");
    if (comando->tabela[0] == '\0')
        return rejeitar(validador, "nome da tabela ausente");

    if (comando->numero_colunas  < 0 || comando->numero_colunas  > MAX_COLUNAS ||
        comando->numero_valores  < 0 || comando->numero_valores  > MAX_VALORES ||
        comando->numero_condicoes < 0 || comando->numero_condicoes > MAX_CONDICOES)
        return rejeitar(validador, "contagem de elementos invalida na AST");

    /* Campos de outro tipo de comando indicam construcao fora do escopo */
    switch (comando->tipo) {
        case CMD_CREATE:
            if (comando->numero_valores || comando->numero_condicoes)
                return rejeitar(validador,
                    "CREATE TABLE nao aceita valores nem condicoes");
            break;
        case CMD_INSERT:
            if (comando->numero_colunas || comando->numero_condicoes)
                return rejeitar(validador,
                    "INSERT nao aceita definicao de colunas nem WHERE");
            break;
        case CMD_SELECT:
            if (comando->numero_colunas || comando->numero_valores)
                return rejeitar(validador,
                    "SELECT so suporta '*' (sem lista de colunas ou valores)");
            break;
    }

    for (int i = 0; i < comando->numero_colunas; i++) {
        const Coluna *c = &comando->colunas[i];
        if (!texto_terminado(c->nome, sizeof c->nome))
            return rejeitar(validador, "nome de coluna malformado na AST");
        if (c->tipo != TOKEN_INT && c->tipo != TOKEN_CHAR)
            return rejeitar(validador, "coluna '%s': tipo '%s' nao suportado",
                            c->nome, nome_token(c->tipo));
    }

    for (int i = 0; i < comando->numero_valores; i++)
        if (!validar_valor_ast(validador, &comando->valores[i], "INSERT"))
            return 0;

    for (int i = 0; i < comando->numero_condicoes; i++) {
        const Condicao *cond = &comando->condicoes[i];

        if (!texto_terminado(cond->coluna, sizeof cond->coluna))
            return rejeitar(validador, "WHERE: nome de coluna malformado na AST");
        if (!e_operador(cond->operacao))
            return rejeitar(validador, "WHERE: operador nao suportado (%s)",
                            nome_token(cond->operacao));
        if (!validar_valor_ast(validador, &cond->valor, "WHERE"))
            return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Etapas 3 a 5: um validador por tipo de comando                      */
/* ------------------------------------------------------------------ */

/*
 * CREATE TABLE: roda as regras do catalogo (nome, duplicidade, limites,
 * tamanho do registro, restricao da arvore B+) sobre uma COPIA do catalogo.
 * Assim as regras nao sao duplicadas aqui e o catalogo real nao e alterado.
 */
static int validar_create(Validador *validador, const Catalogo *cat,
                          const Comando *comando, ConsultaValidada *saida)
{
    Catalogo *copia = malloc(sizeof *copia);
    if (!copia)
        return rejeitar(validador, "memoria insuficiente para validar CREATE TABLE");
    memcpy(copia, cat, sizeof *copia);

    if (!catalogo_adicionar_tabela(copia, comando->tabela, comando->colunas,
                                   comando->numero_colunas)) {
        rejeitar(validador, "CREATE TABLE: %s", copia->mensagem);
        free(copia);
        return 0;
    }

    saida->id_tabela   = cat->numero_tabelas;
    saida->tabela      = NULL;
    saida->nova_tabela = copia->tabelas[cat->numero_tabelas];
    free(copia);
    return 1;
}

static int validar_insert(Validador *validador, Catalogo *cat,
                          const Comando *comando, ConsultaValidada *saida)
{
    const TabelaCat *tabela = catalogo_buscar_tabela(cat, comando->tabela);

    if (!tabela)
        return rejeitar(validador, "tabela '%s' nao existe", comando->tabela);

    if (comando->numero_valores != tabela->numero_colunas)
        return rejeitar(validador, "INSERT em '%s' espera %d valor(es), mas recebeu %d",
                        tabela->nome, tabela->numero_colunas, comando->numero_valores);

    for (int i = 0; i < comando->numero_valores; i++) {
        const ColunaCat *coluna = &tabela->colunas[i];

        if (!valor_compativel(cat, tabela, coluna, &comando->valores[i], "INSERT"))
            return rejeitar(validador, "%s", cat->mensagem);

        saida->valores[i].id_coluna    = i;
        saida->valores[i].tipo_coluna  = coluna->tipo;
        saida->valores[i].tamanho      = coluna->tamanho;
        saida->valores[i].deslocamento = coluna->deslocamento;
        saida->valores[i].valor        = comando->valores[i];
    }

    saida->numero_valores = comando->numero_valores;
    saida->tabela         = tabela;
    saida->id_tabela      = (int)(tabela - cat->tabelas);
    return 1;
}

static int validar_select(Validador *validador, Catalogo *cat,
                          const Comando *comando, ConsultaValidada *saida)
{
    const TabelaCat *tabela = catalogo_buscar_tabela(cat, comando->tabela);

    if (!tabela)
        return rejeitar(validador, "tabela '%s' nao existe", comando->tabela);

    for (int i = 0; i < comando->numero_condicoes; i++) {
        const Condicao *cond = &comando->condicoes[i];
        int indice = tabela_buscar_coluna(tabela, cond->coluna);

        if (indice < 0)
            return rejeitar(validador, "WHERE: coluna '%s' nao existe na tabela '%s'",
                            cond->coluna, tabela->nome);

        const ColunaCat *coluna = &tabela->colunas[indice];

        if (!operador_aplicavel(coluna->tipo, cond->operacao))
            return rejeitar(validador,
                            "WHERE: operador '%s' nao se aplica a coluna '%s' (%s)",
                            simbolo_operacao(cond->operacao), coluna->nome,
                            nome_token(coluna->tipo));

        if (!valor_compativel(cat, tabela, coluna, &cond->valor, "WHERE"))
            return rejeitar(validador, "%s", cat->mensagem);

        saida->condicoes[i].id_coluna    = indice;
        saida->condicoes[i].tipo_coluna  = coluna->tipo;
        saida->condicoes[i].tamanho      = coluna->tamanho;
        saida->condicoes[i].deslocamento = coluna->deslocamento;
        saida->condicoes[i].operacao     = cond->operacao;
        saida->condicoes[i].valor        = cond->valor;
    }

    saida->numero_condicoes = comando->numero_condicoes;
    saida->tabela           = tabela;
    saida->id_tabela        = (int)(tabela - cat->tabelas);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Funcao principal do validador                                       */
/* ------------------------------------------------------------------ */

/*
 * Valida um comando ja aceito pelo parser.
 *   1 = aceito  (saida->valida == 1 e saida esta preenchida)
 *   0 = rejeitado (saida zerada; motivo em validador->mensagem)
 * O catalogo nao e modificado (o ponteiro nao e const apenas porque as
 * funcoes de checagem do catalogo registram sua mensagem nele).
 */
static int validar_consulta(Validador *validador, Catalogo *cat,
                            const Comando *comando, ConsultaValidada *saida)
{
    int ok = 0;

    memset(saida, 0, sizeof *saida);
    validador->mensagem[0] = '\0';

    if (!validar_estrutura(validador, comando))
        return 0;

    saida->tipo = comando->tipo;

    switch (comando->tipo) {
        case CMD_CREATE: ok = validar_create(validador, cat, comando, saida); break;
        case CMD_INSERT: ok = validar_insert(validador, cat, comando, saida); break;
        case CMD_SELECT: ok = validar_select(validador, cat, comando, saida); break;
    }

    if (!ok) {
        memset(saida, 0, sizeof *saida);   /* nada de resultado parcial */
        return 0;
    }
    saida->valida = 1;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Impressao da representacao validada                                 */
/* ------------------------------------------------------------------ */

static void imprimir_consulta_validada(const ConsultaValidada *q)
{
    switch (q->tipo) {
        case CMD_CREATE:
            printf("  CREATE TABLE '%s' -> id_tabela %d (nova)\n",
                   q->nova_tabela.nome, q->id_tabela);
            for (int i = 0; i < q->nova_tabela.numero_colunas; i++) {
                const ColunaCat *c = &q->nova_tabela.colunas[i];
                printf("    coluna #%d %-12s %-4s bytes %d..%d\n", i, c->nome,
                       nome_token(c->tipo), c->deslocamento,
                       c->deslocamento + c->tamanho - 1);
            }
            break;

        case CMD_INSERT:
            printf("  INSERT -> id_tabela %d ('%s')\n", q->id_tabela, q->tabela->nome);
            for (int i = 0; i < q->numero_valores; i++) {
                const ValorValidado *v = &q->valores[i];
                printf("    coluna #%d %-12s %-4s <- ", v->id_coluna,
                       q->tabela->colunas[v->id_coluna].nome,
                       nome_token(v->tipo_coluna));
                imprimir_valor(&v->valor);
                printf("\n");
            }
            break;

        case CMD_SELECT:
            printf("  SELECT * -> id_tabela %d ('%s')\n", q->id_tabela, q->tabela->nome);
            if (q->numero_condicoes == 0)
                printf("    where: (nenhum)\n");
            for (int i = 0; i < q->numero_condicoes; i++) {
                const CondicaoValidada *c = &q->condicoes[i];
                printf("    %s coluna #%d %-12s %-4s %s ", i ? "AND" : "   ",
                       c->id_coluna, q->tabela->colunas[c->id_coluna].nome,
                       nome_token(c->tipo_coluna), simbolo_operacao(c->operacao));
                imprimir_valor(&c->valor);
                printf("\n");
            }
            break;
    }
}

/* ------------------------------------------------------------------ */
/* Bloco main (simulacao: valida, mas nunca executa nem grava)         */
/* ------------------------------------------------------------------ */

#ifndef VALIDADOR_SEM_MAIN

static const char EXEMPLO[] =
    "CREATE TABLE alunos (id INT, nome CHAR);\n"
    "INSERT INTO alunos VALUES (1, 'ana');\n"
    "SELECT * FROM alunos WHERE id >= 1 AND nome <> 'bob';\n"
    "INSERT INTO alunos VALUES ('x', 'ana');\n"
    "INSERT INTO alunos VALUES (2, 'abcde');\n"
    "INSERT INTO alunos VALUES (1);\n"
    "SELECT * FROM inexistente;\n"
    "SELECT * FROM alunos WHERE idade = 3;\n"
    "SELECT * FROM alunos WHERE id = 'x';\n"
    "SELECT * FROM alunos ORDER BY id;\n"
    "SELECT * FROM alunos WHERE id = 1 OR id = 2;\n"
    "CREATE TABLE alunos (id INT, nome CHAR);\n"
    "CREATE TABLE grande (a INT, b INT, c INT);\n";

static char *ler_arquivo(const char *caminho)
{
    FILE *f = fopen(caminho, "rb");
    if (!f)
        return NULL;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long tamanho = ftell(f);
    if (tamanho < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }

    char *texto = malloc((size_t)tamanho + 1);
    if (!texto) { fclose(f); return NULL; }

    size_t lidos = fread(texto, 1, (size_t)tamanho, f);
    texto[lidos] = '\0';
    fclose(f);
    return texto;
}

/*
 * Parseia e valida cada comando do texto. Comandos CREATE aceitos sao
 * registrados APENAS na copia em memoria do catalogo, para que os comandos
 * seguintes do script enxerguem a tabela. Devolve quantos foram rejeitados.
 */
static int processar_script(Catalogo *cat, const char *texto)
{
    Parser parser;
    Validador validador;
    int numero = 0, rejeitados = 0;

    iniciar_parser(&parser, texto);

    while (parser.atual.tipo != TOKEN_FIM) {
        Comando          comando;
        ConsultaValidada validada;

        numero++;

        if (!parse_comando(&parser, &comando)) {
            printf("[%d] REJEITADO (sintaxe): %s\n", numero, parser.mensagem);
            rejeitados++;
            sincronizar(&parser);
            continue;
        }

        if (!validar_consulta(&validador, cat, &comando, &validada)) {
            printf("[%d] REJEITADO (semantica): %s\n", numero, validador.mensagem);
            rejeitados++;
            continue;
        }

        printf("[%d] OK\n", numero);
        imprimir_consulta_validada(&validada);

        if (validada.tipo == CMD_CREATE)
            cat->tabelas[cat->numero_tabelas++] = validada.nova_tabela;
    }

    printf("\n%d comando(s), %d rejeitado(s).\n", numero, rejeitados);
    return rejeitados;
}

int main(int argc, char **argv)
{
    static Catalogo cat;            /* grande: fora da pilha */
    const char *script  = (argc > 1) ? argv[1] : NULL;
    const char *arq_cat = (argc > 2) ? argv[2] : NULL;
    char *texto = NULL;
    int rejeitados;

    catalogo_iniciar(&cat);

    if (arq_cat) {
        int r = catalogo_carregar(&cat, arq_cat);
        if (r < 0) {
            fprintf(stderr, "Catalogo invalido '%s': %s\n", arq_cat, cat.mensagem);
            return 2;
        }
        if (r == 0)
            printf("Arquivo de catalogo '%s' nao existe: partindo de catalogo vazio.\n",
                   arq_cat);
    }

    if (script) {
        texto = ler_arquivo(script);
        if (!texto) {
            fprintf(stderr, "Nao foi possivel ler '%s'.\n", script);
            return 2;
        }
    }

    rejeitados = processar_script(&cat, texto ? texto : EXEMPLO);
    free(texto);

    return rejeitados ? 1 : 0;
}

#endif /* VALIDADOR_SEM_MAIN */
