/* Teste de estresse do Execution Plan: corretude (Index == Table Scan) e qualidade da decisao.
 * Compile a partir da raiz do projeto, ajustando o include abaixo ao seu layout:
 *   gcc -Wall -Wextra -Isrc -o teste teste_execution_plan.c */
#include "../../modules/executor/plan/execution_plan.c"

typedef struct { int32_t ids[4096]; int n; } Ids;

static long executar(Catalogo *cat, const PlanoExecucao *p, Ids *out) {
    CursorPlano c; Row r; int rc;
    long antes = (long)contador_leituras_disco;
    out->n = 0;
    if (plano_abrir(&c, cat, p) != 0) return -1;
    while ((rc = plano_next(&c, &r)) == 1) {
        int32_t id = 0; row_obter_int(&r, 0, &id);
        if (out->n < 4096) out->ids[out->n] = id;
        out->n++;
    }
    plano_fechar(&c);
    if (rc < 0) return -1;
    return (long)contador_leituras_disco - antes;
}

static int contar_folhas(Catalogo *cat, const TabelaCat *t, int *folhas, int *linhas) {
    No no; int pg;
    if (!selecionar_arquivo_tabela(cat, t)) return -1;
    pg = ler_raiz();
    while (1) { if (ler_no(pg, &no)) return -1; if (no.tipo == NO_FOLHA) break; pg = no.filhos[0]; }
    *folhas = 0; *linhas = 0;
    while (1) {
        (*folhas)++; *linhas += no.numero_chaves;
        if (no.proxima == SEM_PAGINA) break;
        if (ler_no(no.proxima, &no)) return -1;
    }
    return 0;
}

static unsigned long rng = 12345;
static int aleat(int m) { rng = rng * 6364136223846793005UL + 1442695040888963407UL; return (int)((rng >> 33) % (unsigned long)m); }

static int rodar(int N, int embaralhar) {
    static Catalogo cat; Coluna col[2]; Row linha;
    remove("alunos.dat");
    catalogo_iniciar(&cat);
    strcpy(col[0].nome, "id"); col[0].tipo = TOKEN_INT;
    strcpy(col[1].nome, "nome"); col[1].tipo = TOKEN_CHAR;
    catalogo_adicionar_tabela(&cat, "alunos", col, 2);
    const TabelaCat *t = catalogo_buscar_tabela(&cat, "alunos");
    for (int i = 0; i < N; i++) {
        int id = embaralhar ? (int)(((long)i * 7919) % N) + 1 : i + 1;
        row_iniciar(&linha, t); row_definir_int(&linha, 0, id); row_definir_char(&linha, 1, "x");
        if (row_inserir(&cat, &linha) != INSERE_OK) { printf("falha insert\n"); return 1; }
    }
    int fr, lr; contar_folhas(&cat, t, &fr, &lr);

    /* estatisticas estimadas x reais */
    Parser ps; Comando cmd; PlanoExecucao pl;
    iniciar_parser(&ps, "SELECT * FROM alunos WHERE id >= 1;"); parse_comando(&ps, &cmd);
    plano_criar(&pl, &cat, &cmd);
    printf("N=%-5d %-10s folhas real=%d est=%d | linhas real=%d est=%.0f | altura=%d\n",
           N, embaralhar ? "embaralhado" : "ordenado", fr, pl.stats.folhas, lr, pl.stats.linhas, pl.stats.altura);

    int total = 0, divergentes = 0, certos = 0, escolheu_idx = 0;
    long regret = 0, melhor_total = 0, escolhido_total = 0;
    for (int q = 0; q < 300; q++) {
        char sql[160];
        int a = aleat(N) + 1, largura;
        int tipo = aleat(4);
        if (tipo == 0)      snprintf(sql, sizeof sql, "SELECT * FROM alunos WHERE id = %d;", a);
        else if (tipo == 1) { largura = aleat(N / 4 + 1); snprintf(sql, sizeof sql, "SELECT * FROM alunos WHERE id >= %d AND id <= %d;", a, a + largura); }
        else if (tipo == 2) snprintf(sql, sizeof sql, "SELECT * FROM alunos WHERE id > %d;", a);
        else                { largura = aleat(N); snprintf(sql, sizeof sql, "SELECT * FROM alunos WHERE id >= %d AND id < %d;", a, a + largura); }
        iniciar_parser(&ps, sql);
        if (!parse_comando(&ps, &cmd)) { printf("parse falhou: %s\n", sql); return 1; }
        if (plano_criar(&pl, &cat, &cmd) != 0) { printf("plano falhou: %s\n", pl.mensagem); return 1; }
        PlanoExecucao alt = pl;
        alt.metodo = pl.metodo == ACESSO_INDEX_SCAN ? ACESSO_TABLE_SCAN : ACESSO_INDEX_SCAN;
        Ids x, y;
        long lx = executar(&cat, &pl, &x), ly = executar(&cat, &alt, &y);
        if (lx < 0 || ly < 0) { printf("execucao falhou: %s\n", sql); return 1; }
        int nn = x.n < 4096 ? x.n : 4096;
        if (x.n != y.n || memcmp(x.ids, y.ids, sizeof(int32_t) * (size_t)nn)) { divergentes++; printf("DIVERGE: %s\n", sql); }
        total++;
        if (pl.metodo == ACESSO_INDEX_SCAN) escolheu_idx++;
        long melhor = lx < ly ? lx : ly;
        if (lx <= ly) certos++;
        regret += lx - melhor; melhor_total += melhor; escolhido_total += lx;
    }
    printf("   %d consultas | divergencias=%d | escolheu Index=%d | decisao otima=%d (%.0f%%) | leituras escolhido=%ld vs otimo=%ld (+%.1f%%)\n",
           total, divergentes, escolheu_idx, certos, 100.0 * certos / total, escolhido_total, melhor_total,
           melhor_total ? 100.0 * regret / melhor_total : 0.0);
    printf("   reiniciar_cache -> %s\n", reiniciar_cache() == 0 ? "ok (sem pin vazado)" : "PIN VAZADO");
    remove("alunos.dat");
    return divergentes;
}

int main(void) {
    int bad = 0;
    int tamanhos[] = { 5, 20, 60, 500, 3000 };
    for (int i = 0; i < 5; i++) { bad += rodar(tamanhos[i], 1); bad += rodar(tamanhos[i], 0); }
    printf("\nTOTAL divergencias: %d\n", bad);
    return bad != 0;
}
