# Módulo 1

## Introdução

O Módulo 1 se trata de um **Sistema de Armazenamento em Páginas**,o qual é responsável por alocar os bytes para unidades chamadas de Páginas,as quais são usadas pelos módulos superiores (`Parser,Catálogo,Executor`) para realizarem operações mais abstratas.

O Módulo 1 inicialmente se tratava apenas páginas de 4KB,cabeçalhos de 16B e registros de 8B,possuindo somente funções de `ler_pagina()` e `escrever_pagina()`.No entanto,à medida em que requisitos como Persistência e Isolamento foram sendo solicitados,a complexidade do módulo tornou-se cada vez maior.

Essa seção chamada de Módulo 1 tem como objetivo discutir as funcionalidades implementadas e as decisões tomadas pela equipe,a fim de fornecer um pequeno **relatório** sobre o desenvolvimento do primeiro componente do projeto.

## Descrição

### Arquitetura Geral:

O sistema modela o arquivo `craft-db.dat` como um array de páginas de tamanho fixo.A página n sempre começa no byte `n * 4096` do arquivo.Dessa forma,é possível ter um acesso direto a qualquer página,usando apenas a **aritmética do endereço**.

Cada página tem 4096 bytes (4KB),os quais são divididos em:

1.**Cabeçalho**:Contém os primeiros 16 bytes,sendo reservados para os metadados.

2.**Registros**:É a área de 4080 bytes organizada em 510 slots de 8 bytes.

### Leitura e Escrita de Páginas:

Representam as duas funções centrais do sistema.Ambas calculam o offset da página (`n * TAMANHO_PAGINA`),posicionam o cursor do arquivo nesse ponto com `fseek`,e então transferem exatamente 4096 bytes com `fread`/`fwrite`.Não há necessidade de percorrer o arquivo, nem de manter índices auxiliares: a própria aritmética do offset *é* o índice.

``` C
 void ler_pagina(int n, unsigned char *buffer) {
    FILE *f = fopen(ARQUIVO_BD, "rb");
    if (f == NULL) {
        memset(buffer, 0, TAMANHO_PAGINA);
        return;
    }
    fseek(f, (long)n * TAMANHO_PAGINA, SEEK_SET);
    fread(buffer, 1, TAMANHO_PAGINA, f);
    fclose(f);
}

void escreve_pagina(int n, unsigned char *buffer) {
    FILE *f = fopen(ARQUIVO_BD, "r+b");
    if (f == NULL) {
        f = fopen(ARQUIVO_BD, "w+b");
    }
    fseek(f, (long)n * TAMANHO_PAGINA, SEEK_SET);
    fwrite(buffer, 1, TAMANHO_PAGINA, f);
    fclose(f);
}
```

A sua complexidade é de `O(1)` em relação ao número de páginas do banco,pois ler ou escrever na página 5 custa o mesmo do que escrever na página 5.000.000,pois `fseek` é uma operação de custo constante no sistema operacional.

### Persistência em Disco:

A operação de gravação em C passa por pelo menos duas camadas de buffer antes de chegar ao disco:o buffer da biblioteca padrão C (que está dentro do processo) e o cache de páginas do sistema operacional (que está fora do processo,mas ainda dentro da RAM).

```C
void escreve_pagina(int n, unsigned char *buffer) {
    FILE *f = fopen(ARQUIVO_BD, "r+b");
    if (f == NULL) {
        f = fopen(ARQUIVO_BD, "w+b");
    }
    fseek(f, (long)n * TAMANHO_PAGINA, SEEK_SET);
    fwrite(buffer, 1, TAMANHO_PAGINA, f);

    fflush(f);         /* Envia os dados do buffer da biblioteca C para o SO   */
    fsync(fileno(f));  /* Força o SO a gravar os dados fisicamente no disco    */

    fclose(f);
}
```
O `fwrite` apenas garante que os dados saíram do buffer do C.O `fflush` os empurra para o SO.Já o `fsync` é o único que força o SO a efetivamente gravá-los no disco físico,realizando um bloqueio até que isso aconteça.

Em termos de custo computacional,a `fsync` é a operação mais cara de toda a biblioteca pois envolve acesso físico ao disco.

### Validação do Buffer e do Número da Página:

Como o C não carrega a informação do tamanho junto com ponteiros,as funções de leitura e escrita passaram a receber explicitamente o tamanho do buffer e validá-lo em relação ao valor de 4KB,além de verificar se o ponteiro é `NULL`.

``` C
int ler_pagina(int n, unsigned char *buffer, size_t tamanho_buffer) {
    if (buffer == NULL || tamanho_buffer != TAMANHO_PAGINA) {
        fprintf(stderr, "Erro: buffer invalido (deve ter exatamente %d bytes)\n", TAMANHO_PAGINA);
        return -1;
    }
    if (n < 0 || n >= total_paginas_alocadas()) {
        fprintf(stderr, "Erro: numero de pagina invalido (%d). Paginas alocadas: %d\n",
                n, total_paginas_alocadas());
        return -1;
    }

    FILE *f = fopen(ARQUIVO_BD, "rb");
    if (f == NULL) {
        fprintf(stderr, "Erro: nao foi possivel abrir '%s'\n", ARQUIVO_BD);
        return -1;
    }
    fseek(f, (long)n * TAMANHO_PAGINA, SEEK_SET);
    if (fread(buffer, 1, TAMANHO_PAGINA, f) != TAMANHO_PAGINA) {
        fprintf(stderr, "Erro: falha ao ler a pagina %d\n", n);
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

int escreve_pagina(int n, unsigned char *buffer, size_t tamanho_buffer) {
    if (buffer == NULL || tamanho_buffer != TAMANHO_PAGINA) {
        fprintf(stderr, "Erro: buffer invalido (deve ter exatamente %d bytes)\n", TAMANHO_PAGINA);
        return -1;
    }
    if (n < 0 || n >= total_paginas_alocadas()) {
        fprintf(stderr, "Erro: pagina %d nao foi alocada. Use aloca_pagina() antes de escrever.\n", n);
        return -1;
    }

    return escreve_pagina_bruta(n, buffer);
}
```

O número da página é validado contra o intervalo `[0,total_paginas_alocadas()]`,dessa forma rejeitando páginas com valores negativos ou que ainda não foram alocadas.

### Sincronização de Dados:

Extrai a lógica de `fflush` + `fsync` em uma função pública e independente, que pode ser chamada a qualquer momento — por exemplo, como um "ponto de checkpoint" após uma sequência de operações, sem depender de que cada `escreve_pagina` já sincronize individualmente.

``` C
int sincroniza_dados(void) {
    FILE *f = fopen(ARQUIVO_BD, "r+b");
    if (f == NULL) {
        fprintf(stderr, "Erro: nao foi possivel abrir '%s' para sincronizar\n", ARQUIVO_BD);
        return -1;
    }
    fflush(f);
    if (fsync(fileno(f)) != 0) {
        fprintf(stderr, "Erro: falha ao sincronizar dados com o disco\n");
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}
``` 

### Alocação de Página:

``` C
int total_paginas_alocadas(void) {
    return (int)(tamanho_arquivo() / TAMANHO_PAGINA);
}

int aloca_pagina(void) {
    int nova_pagina = total_paginas_alocadas();
    unsigned char pagina_vazia[TAMANHO_PAGINA];
    memset(pagina_vazia, 0, TAMANHO_PAGINA);

    if (escreve_pagina_bruta(nova_pagina, pagina_vazia) != 0) {
        return -1;
    }
    return nova_pagina;
}
```

A função `total_paginas_alocadas()` calcula quantas páginas já existem dividindo o tamanho atual do arquivo (o qual é obtido com `fseek` + `ftell()`) por 4096.

A função `aloca_pagina()` usa esse valor para saber qual é o próximo número da página livre,escreve uma página zerada exatamente nessa posição (o que estende o arquivo) e devolve esse número.

A função `escreve_pagina()` exige que a página já esteja alocada,porém a função `aloca_pagina()` precisa escrever a nova página quando ela ainda não está alocada.Para resolver isso,foi necessário criar uma função chamada `escreve_pagina_buffer`,a qual faz a escrita sem fazer essa verificação.

``` C
int escreve_pagina(int n, unsigned char *buffer, size_t tamanho_buffer) {
    if (buffer == NULL || tamanho_buffer != TAMANHO_PAGINA) {
        fprintf(stderr, "Erro: buffer invalido (deve ter exatamente %d bytes)\n", TAMANHO_PAGINA);
        return -1;
    }
    if (n < 0 || n >= total_paginas_alocadas()) {
        fprintf(stderr, "Erro: pagina %d nao foi alocada. Use aloca_pagina() antes de escrever.\n", n);
        return -1;
    }

    return escreve_pagina_buffer(n, buffer);
```

### Isolamento entre Páginas:

Cada operação de leitura ou escrita passou a adquirir, via `fcntl(F_SETLKW)`, uma trava (`struct flock`) restrita exatamente ao **intervalo de bytes** da página envolvida (`[n*4096, n*4096+4095]`), liberando-a logo em seguida.Como a trava cobre só esse intervalo, uma escrita na página 7 nunca disputa, nem pode se sobrepor a, nenhuma trava relativa à página 8.

``` C
static int trava_pagina(FILE *f, int n, short tipo_trava) {
    struct flock trava;
    memset(&trava, 0, sizeof(trava));
    trava.l_type   = tipo_trava;
    trava.l_whence = SEEK_SET;
    trava.l_start  = (long)n * TAMANHO_PAGINA;
    trava.l_len    = TAMANHO_PAGINA;

    if (fcntl(fileno(f), F_SETLKW, &trava) == -1) {
        fprintf(stderr, "Erro: falha ao travar a pagina %d\n", n);
        return -1;
    }
    return 0;
}
```

De todas as funcionalidades,esta foi **a mais difícil de ser implementada**,não pela quantidade de código (já que possuí poucas linhas),mas pela natureza do problema,pois é a única funcionalidade cuja correção depende da *ordem de execução entre processos diferentes*,e não apenas da lógica sequencial dentro de uma única chamada. Além disso, travas POSIX via `fcntl` têm uma armadilha conhecida:elas são associadas ao par (processo, arquivo), não ao descritor específico, de modo que fechar *qualquer* descritor daquele arquivo no processo libera *todas* as travas que o processo detém sobre ele. Isso obrigou a manter rigorosamente o padrão "abrir → travar → operar → destravar → fechar" em uma única sequência ininterrupta, sem descritores duplicados abertos ao mesmo tempo. A correção também não é visualmente óbvia ao rodar o programa normalmente: um erro sutil no cálculo do intervalo da trava poderia deixar duas páginas se sobrepondo sem que isso gerasse nenhum sintoma evidente — por isso a verificação, nesse caso, exigiu inspecionar os bytes crus do arquivo (com `dd`/`od`), e não apenas confiar na saída do programa.

### Serialização e Desserialização:

Até esta etapa, um registro era tratado apenas como 8 bytes crus e opacos. Foi introduzido o tipo `Registro` (`id` + `valor`, dois inteiros de 4 bytes), junto com funções que convertem esse `struct` para exatamente 8 bytes em **little-endian** — e vice-versa — usando deslocamento de bits (`& 0xFF`, `>> 8`, `>> 16`, `>> 24`) byte a byte, em vez de depender de `memcpy` direto do `struct` ou de macros de endianness do sistema.

``` C
static void serializa_uint32_le(uint32_t valor, unsigned char *destino) {
    destino[0] = (unsigned char)(valor & 0xFF);
    destino[1] = (unsigned char)((valor >> 8) & 0xFF);
    destino[2] = (unsigned char)((valor >> 16) & 0xFF);
    destino[3] = (unsigned char)((valor >> 24) & 0xFF);
}

static uint32_t desserializa_uint32_le(const unsigned char *origem) {
    return   (uint32_t)origem[0]
           | ((uint32_t)origem[1] << 8)
           | ((uint32_t)origem[2] << 16)
           | ((uint32_t)origem[3] << 24);
}
```

O `hexdump` é a forma de representar os dados binários (como arquivos,blocos de memória) em formato hexadecimal,sendo acompanhada por um texto `ASCII`.

Em vez de exibir dados brutos de forma ininteligível, o hexdump divide o conteúdo em linhas que mostram:

O endereço do byte inicial (offset).

Os valores dos bytes em hexadecimal (cada byte de 00 a FF).

A representação em caracteres imprimíveis correspondentes à direita.

A relação entre o hexdump e a serialização e desserialização está na inspeção e no entendimento dos dados brutos.Como a serialização resulta em um fluxo de bytes, o hexdump serve como uma ferramenta de diagnóstico essencial.

Ao rodar o comando `hexdump -C -s 8192 -n craft-db.dat` é possível observar os seguintes dados:

``` Shell
00002000  00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00  |................|
00002010  01 02 03 04 05 06 07 08  00 00 00 00 00 00 00 00  |................|
00002020
```
O valor `00002000` representa **8192** em hexadecimal,sendo o começo da página 2.Já o valor `00002010` representa o byte **8208**.

## Suíte de Testes

Encontra-se no diretório `Test/M1`.

Nela,temos um arquivo `M1.c` independente que está ligado à biblioteca `Header.h`,sendo usado por `Main.c`. Esses arquivos estão separados devido a tanto `Test.c` quanto `Main.c` possuirem seu próprio bloco `main`,impossibilitando uma compilação direta.

Para compilar e executar os arquivos,rode os seguintes comandos:

``` shell
gcc M1.c Main.c -o Main # Linka o Main.c com o M1.c e compila 
```


``` shell
gcc M1.c Test.c -o Test # Linka o Test.c com o M1.c,compila e executa o main do Test.c
```

Cada funcionalidade ganhou uma função de teste dedicada (alocação, validação, leitura/escrita, isolamento, serialização, sincronização, persistência), usando uma pequena infraestrutura própria (`verifica`/`VERIFICA`) que registra sucesso ou falha sem interromper a execução dos demais testes.

A maior parte dos testes é direta,tratando-se de configurar um estado,chamar a função,comparar o resultado esperado.
 
O teste de **persistência**, porém, teve uma complexidade própria:para provar de verdade que os dados sobrevivem ao fim da execução (e não apenas "parecem" persistir porque o mesmo processo ainda está de pé), foi necessário lançar, via `popen`, um **processo filho genuinamente novo**,o qual não possuí nenhum estado em memória herdado,relendo o arquivo do zero e devolvendo o resultado ao processo pai. Esse é o único teste do projeto cuja validação depende de orquestrar múltiplos processos, e não apenas de chamar funções dentro do mesmo programa.

## Conclusões

A complexidade **não cresceu de forma computacional**:todas as operações continuam O(1),porque o modelo de páginas de tamanho fixo com acesso por offset é, por construção, um modelo de custo constante. O que cresceu foi a complexidade **de raciocínio**:cada nova funcionalidade não adicionou apenas código, mas uma nova *garantia* que o restante do sistema passou a poder assumir como verdadeira — que o buffer tem o tamanho certo, que a página existe, que os dados realmente chegaram ao disco, que duas escritas concorrentes não se pisam, que o formato em disco não depende da máquina que o escreveu.

Essa progressão espelha, em miniatura, como sistemas de armazenamento reais evoluem,indo de um mecanismo bruto de leitura/escrita, para uma camada com validação defensiva, depois para uma camada com garantias de durabilidade, depois para gerenciamento de espaço (alocação), depois para segurança sob concorrência, e só então para um formato de dados portátil e testável.

# Módulo 2

## Introdução

O Módulo 2 implementa Cache seguindo as políticas **STEAL** e **NO-FORCE**.A sua principal função é acelerar as leituras realizadas pelo CraftDB aos frames das páginas do Módulo 1.

Inicialmente,o Módulo 2 só implementou a estrutura de frames e cache de páginas adotando as políticas STEAL e NO-FORCE.

Durante o desenvolvimento do Módulo foram definidos 4 frames,a fim de validar a função `_expulsa()`.

A decisão de seguir as políticas STEAL e NO-FORCE foi tomada a fim de garantir que o Cache possa gravar as páginas sujas no disco antes de um `COMMIT`,o qual pode gravá-las **posteriormente**. 

Além disso,o CraftDB implementará `Logs` no Módulo 6.

## Descrição

### Visão Geral

O Módulo 2 adota três políticas:

1. Política de Substituição LRU (**Least Recently Used**):É usada para escolher as páginas candidatas à expulsão

2. Política STEAL:É usada para expulsar páginas sujas desde que elas estejam gravadas no disco antes da expulsão
 
3. Política NO-FORCE:É usada para evitar a gravação obrigatória de uma página no momento da expulsão.

Isso permite que o Módulo controle o número de fixações de cada página,o estado de sujeira,a quantidade de acertos e erros de cache e o momento lógico do seu último uso.

O Módulo implementa um `Buffer Pool` com capacidade configurável em tempo de compilação.O arquivo `M2.c` implementou uma capacidade de 4 frames a fim de testar a política LRU.

### Frames

``` C
typedef struct {
    int pagina;                           
    unsigned char dados[TAMANHO_PAGINA];   
    int fixacoes;                        
    int suja;                           
    long ultimo_uso;                    
} Frame;
```

Cada página ocupa um frame. 

Eles são responsáveis por armazenar os dados temporariamente de acordo com o número da página,o contador de fixações,o indicador de página suja e o timestamp lógico do último uso.

### Cache de Páginas

``` C
typedef struct {
    Frame frames[NUM_FRAMES];
    int capacidade;   
    int uso;          
    long relogio;      
    long acertos;      
    long faltas;      
} CachePaginas;
```

É a estrutura do Cache.

Ela armazena um array de frames disponíveis para armazenar páginas,a quantidade máxima de páginas que podem permanecer residentes,a quantidade atual de frames ocupados,a quantidade de acertos e erros,e o contador lógico usado pelo LRU.

### Gravação de Página Suja

``` C
int descarrega(int n) {
    inicializa_cache();
    int idx = busca_frame(n);
    if (idx == -1) {
        return 0; 
    }
    if (!cache.frames[idx].suja) {
        return 0; 
    }

    if (escreve_pagina(n, cache.frames[idx].dados, TAMANHO_PAGINA) != 0) {
        fprintf(stderr, "Erro: falha ao descarregar a pagina %d para o disco\n", n);
        return -1;
    }

    cache.frames[idx].suja = 0;
    return 0;
}
```

Grava o frame que está na página suja `n`.

Após a escrita a página deixa de estar suja.

### Pin

``` C
unsigned char *fixa(int n) {
    inicializa_cache();

    if (n < 0 || n >= total_paginas_alocadas()) {
        fprintf(stderr, "Erro: tentativa de fixar pagina invalida (%d)\n", n);
        return NULL;
    }

    int idx = busca_frame(n);
    if (idx != -1) {
        // ACERTO: a pagina ja esta em cache -> devolve a mesma referencia
        cache.acertos++;
        cache.frames[idx].fixacoes++;
        cache.frames[idx].ultimo_uso = ++cache.relogio;
        return cache.frames[idx].dados;
    }

    cache.faltas++;

    if (cache.uso >= cache.capacidade) {
        idx = _expulsa();
        if (idx == -1) {
            return NULL; // cache cheio, todos os frames fixados
        }
    } else {
        for (int i = 0; i < cache.capacidade; i++) {
            if (cache.frames[i].pagina == -1) {
                idx = i;
                break;
            }
        }
    }

    if (ler_pagina(n, cache.frames[idx].dados, TAMANHO_PAGINA) != 0) {
        fprintf(stderr, "Erro: falha ao carregar a pagina %d no cache\n", n);
        return NULL;
    }

    cache.frames[idx].pagina = n;
    cache.frames[idx].suja = 0;
    cache.frames[idx].fixacoes = 1;
    cache.frames[idx].ultimo_uso = ++cache.relogio;
    cache.uso++;

    return cache.frames[idx].dados;
}
```

Fixa a página `n` no Cache e retorna um ponteiro para o seu buffer de 4KB.

Caso a página esteja em algum frame,o contador de fixações é incrementado e a mesma referência é retornada a essa página,em vez de uma cópia nova.

Caso um frame que esteja sendo procurado não seja encontrado,ele é liberado de acordo com as políticas STEAL e NO-FORCE.

### Unpin

``` C
int solta(int n, int suja) {
    inicializa_cache();

    int idx = busca_frame(n);
    if (idx == -1) {
        fprintf(stderr, "Erro: tentativa de soltar a pagina %d, que nao esta em cache\n", n);
        return -1;
    }
    if (cache.frames[idx].fixacoes <= 0) {
        fprintf(stderr, "Erro: pagina %d ja estava solta (fixacoes == 0)\n", n);
        return -1;
    }

    cache.frames[idx].fixacoes--;
    if (suja) {
        cache.frames[idx].suja = 1;
    }
    return 0;
}

```

Decrementa o contador de fixações.

### LRU

``` C
static int _expulsa(void) {
    int idx_vitima = -1;
    long menor_uso = 0;

    for (int i = 0; i < cache.capacidade; i++) {
        if (cache.frames[i].pagina == -1) {
            continue; 
        }
        if (cache.frames[i].fixacoes > 0) {
            continue; 
        }
        if (idx_vitima == -1 || cache.frames[i].ultimo_uso < menor_uso) {
            idx_vitima = i;
            menor_uso = cache.frames[i].ultimo_uso;
        }
    }

    if (idx_vitima == -1) {
        fprintf(stderr, "Erro: cache cheio e nao ha frames elegiveis para expulsao (todos fixados)\n");
        return -1;
    }

    if (cache.frames[idx_vitima].suja) {
        if (descarrega(cache.frames[idx_vitima].pagina) != 0) {
            fprintf(stderr, "Erro: falha ao gravar pagina suja %d antes da expulsao\n",
                    cache.frames[idx_vitima].pagina);
            return -1;
        }
    }

    cache.frames[idx_vitima].pagina = -1;
    cache.frames[idx_vitima].suja = 0;
    cache.frames[idx_vitima].fixacoes = 0;
    cache.uso--;

    return idx_vitima;
}
```

Escolha o frame sem fixações,cuja página foi usada há mais tempo.

Caso a página esteja suja,ela é gravada em disco por meio da função `descarrega()` antes do frame ser liberado,seguindo a política STEAL.

Dentre todas as funcionalidades,esta envolve a decisão mais difícil,a qual se refere à durabilidade do CraftDB.

Uma página fixada não pode ser processada por essa função devido ao bloco `if (cache.frames[i].fixacoes > 0){ continue;}`. Além disso,a política STEAL não lida com erros de E/S que podem ocasionar em falhas de gravação. Sendo assim,o bloco de código `if(descarrega(cache.frames[idx_vitima].pagina) != 0)` aborta a expulsão e retorna -1. Essa decisão favorece a durabilidade,mas prejudica a disponibilidade pois se as páginas sujas não podem ser gravadas em caso de falha de disco,o cache pode ficar temporariamente incapaz de atender novas fixações.

## Suíte de Testes

Localiza-se no diretório `M2` de `Test`.Nela,temos três arquivos C:`T1.c,T2.c,T3.c`.

O `T1.c` testa a capacidade do cache com 4 frames,verificando se múltiplas leituras de uma mesma página resultam em uma única leitura fixa no disco.Nele,a primeira leitura deve resultar em um cache miss e disparar exatamente uma leitura no disco,com as seguintes leituras resultando em cache hits,sem tocar o disco e devolvendo sempre a mesma referência.

O `T2.c` testa o cache com a capacidade reduzida de 3 frames.Ele verifica se ao tocar 4 páginas distintas com um cache de apenas 3 frames,a página que for expulsa será exatamente a usada menos recentemente (LRU),em vez da primeira que entrou (FIFO).

O `T3.c` verifica que uma alteração realizada em uma página após esta ter sido expulsa permanece persistida quando o banco é reaberto em outro processo do sistema operacional.O seu binário possuí dois modos e deve ser executado duas vezes,como dois processos distintos:processo A,que cria o banco,altera uma página,força sua expulsão do cache e encerra;processo B,que abre o mesmo arquivo de banco do zero e confirma que a alteração realizada pelo processo A está lá.

O `Test.sh` é um arquivo de Shell Script que só pode ser executado em ambientes Linux. Ele compila e executa a suíte dos testes de aceitação do M2. Para executá-lo rode o seguinte comando: ` bash Test.sh`.

## Conclusões

O Cache é uma importante etapa no desenvolvimento de um SGBD,pois ele permite que bancos de dados possam acessar arquivos com muito mais eficiência.Ao desenvolver esse módulo foi necessário seguir as políticas STEAL e NO-FORCE,além do LRU.Se não fosse por essas especificações o banco poderia gravar em disco páginas sujas de transações que ainda não realizaram o COMMIT (STEAL) ou não ser capaz de gravar as páginas alteradas por uma transação em um COMMIT (NO-FORCE).Além disso,em caso de cache cheio,usar FIFO em vez de LRU significaria que um cache hit não alteraria a fila,ao contrário do LRU onde cada cache hit devolveria a página para o fim da fila.

# Módulo 3

## Introdução

O Módulo 3 se trata da implementação de uma `Árvore B+` no CraftDB. Essa estrutura é utilizada com o intuito de **indexação**,acelerando a recuperação de dados armazenados no HDD/SSD e minimizando o número de operações de leitura e escrita (I/O).

## Descrição

### Visão Geral

A Árvore B+ guarda dezenas ou centenas de chaves e ponteiros por nó,o que permite alcançar qualquer dado em poucos saltos,mesmo que haja uma tabela com milhões de registros.

Os bancos de dados utilizam a Árvore B+ em buscas por igualdade (`WHERE`) e por intervalo (`BETWEEN`).

Em uma busca que usa o `WHERE`,o banco começa na raiz e navega pelos nós internos comparando valores até chegar ao nó folha correto. Já que a árvore é balanceada,todas as folhas estão na mesma profundidade,o que garante um tempo de busca extremamente rápido(O(Log (N))).

Em uma busca que usa o `BETWEEN`,o banco faz a busca inicial a fim de encontrar o primeiro registro que corresponda ao valor no nó folha. Em seguida,o banco segue os ponteiros horizontais que ligam as folhas vizinhas,em vez de subir de volta pela árvore.

À medida em que registros são inseridos e deletados,a Árvore B+ se auto-balanceia,garantindo que o desempenho do banco não se degrade com o crescimento dos dados.

### Nó

O cabeçalho de um nó é composto por 16 bytes. 

| Bytes | Campo | Descrição |
|---|---|---
| 0–3 | `tipo` | `NO_INTERNO` (0) ou `NO_FOLHA` (1) |
| 4–7 | `num_chaves` | Quantidade de chaves/registros em uso |
| 8–11 | `proxima` | Folha: próxima folha (ou `-1`); interno: não usado |
| 12–15 | reservado | **Livre** |

Existem dois tipos de nós:

1. Nós **Internos**: Armazenam apenas chaves e ponteiros para outros nós. Não guardam os dados reais. A sua única função é guiar a busca de forma eficiente.
2. Nós **Folhas**: Ficam todos no nível mais baixo da árvore. Armazenam todas as chaves de busca junto com os ponteiros para os registros de dados. São interligados sequencialmente por ponteiros.

Cada nó no CraftDB armazena até 3 registros (chaves). Com esse valor,cerca de 99% de cada página permanece vazia.

```C
typedef struct {
    int32_t  tipo;
    int32_t  num_chaves;
    int32_t  proxima;
    int32_t  chaves[MAX_CHAVES + 1];     // só internos
    int32_t  filhos[MAX_CHAVES + 2];     // só internos
    Registro registros[MAX_CHAVES + 1];  // só folhas
} No;
```

### Inserção

É uma função **recursiva**,a qual retorna que um nó foi inserido com sucesso,se houve duplicação,divisão ou um erro.

``` C
static int inserir_registro(int pagina, const Registro *reg, int32_t *chave_promovida, int *nova_pagina) {
    No no;
    if (le_no(pagina, &no) != 0) return INSERE_ERRO;

    if (no.tipo == NO_FOLHA) {
        int pos = 0;
        while (pos < no.num_chaves && no.registros[pos].id < reg->id) {
            pos++;
        }
        if (pos < no.num_chaves && no.registros[pos].id == reg->id) {
            return INSERE_DUPLICADO;
        }

        for (int i = no.num_chaves; i > pos; i--) {
            no.registros[i] = no.registros[i - 1];
        }
        no.registros[pos] = *reg;
        no.num_chaves++;

        if (no.num_chaves <= MAX_CHAVES) {
            return (grava_no(pagina, &no) == 0) ? INSERE_OK : INSERE_ERRO;
        }
        return dividir_folha(pagina, &no, chave_promovida, nova_pagina);
    }

    /* No interno: desce para o filho adequado */
    int i = indice_filho(&no, reg->id);
    int32_t chave_filho;
    int pagina_filho;
    int r = inserir_registro(no.filhos[i], reg, &chave_filho, &pagina_filho);
    if (r != INSERE_DIVIDIU) {
        return r;
    }

    /* O filho dividiu: insere a chave de roteamento e o novo ponteiro aqui */
    for (int j = no.num_chaves; j > i; j--) {
        no.chaves[j] = no.chaves[j - 1];
    }
    for (int j = no.num_chaves + 1; j > i + 1; j--) {
        no.filhos[j] = no.filhos[j - 1];
    }
    no.chaves[i]     = chave_filho;
    no.filhos[i + 1] = pagina_filho;
    no.num_chaves++;

    if (no.num_chaves <= MAX_CHAVES) {
        return (grava_no(pagina, &no) == 0) ? INSERE_OK : INSERE_ERRO;
    }
    return dividir_interno(pagina, &no, chave_promovida, nova_pagina);
}
```

Essa foi a funcionalidade mais complexa de se implementar devido aos seguintes fatores:

1. **Aritmética dos índices em vetores com tamanhos diferentes**: Em um nó interno com n chaves existem n+1 filhos. Quando se insere uma chave de um filho dividido,as chaves são deslocadas com base no `num_chaves` e os filhos com base no `num_chaves` + 1,com a nova chave ficando em `chaves[i]`,enquanto o novo ponteiro vai para `filhos[i+1]`,em vez de `i+1`. Caso ocorra um erro no cálculo do índice (i+1),a árvore é corrompida silenciosamente,sem causar um travamento no programa.
2. **Divisão do nó interno**: É o que mais exige raciocínio,pois envolve escolher o índice que sobe (`meio = total/2`),calcular o número de chaves e o número de filhos que vão para a direita (`total = meio -1`) e garantir que as entradas restantes no nó esquerdo sejam ignoradas pela serialização.
3. **Recursão com múltiplos retornos**: A função retorna `OK,DUPLICADO,ERRO` ou `DIVIDIU`,porém essa propagação só funciona se cada nível tratar corretamente cada caso.

### Busca

Busca uma chave na árvore,descendo da raiz até a folha. Retorna 0 se encontrou,1 se a chave existe,ou -1 em caso de erro.


``` C
int buscar_valor(int32_t chave, Registro *saida) {
    int pagina = le_raiz();
    if (pagina < 0) return -1;

    No no;
    for (;;) {
        if (le_no(pagina, &no) != 0) return -1;
        if (no.tipo == NO_FOLHA) break;
        pagina = no.filhos[indice_filho(&no, chave)];
    }

    for (int i = 0; i < no.num_chaves; i++) {
        if (no.registros[i].id == chave) {
            if (saida != NULL) *saida = no.registros[i];
            return 0;
        }
    }
    return 1;
}
```

### Divisão

A divisão ocorre quando um nó possuí um número de chaves maior do que o permitido. Existem dois tipos: a divisão de folha e a de nó interno.

#### Divisão de Folha

A folha original fica com os dois primeiros registros. Uma nova folha recebe os dois últimos registros e é encadeada logo após a original.

Retorna `INSERE_DIVIDIU` em caso de sucesso ou `INSERE_ERRO`.

``` C
int dividir_folha(int pagina, No *no, int32_t *chave_promovida, int *nova_pagina) {
    int nova = aloca_pagina();
    if (nova < 0) return INSERE_ERRO;

    int total = no->num_chaves;          
    int meio  = (total + 1) / 2;         

    No dir;
    memset(&dir, 0, sizeof(dir));
    dir.tipo       = NO_FOLHA;
    dir.num_chaves = total - meio;
    for (int i = 0; i < dir.num_chaves; i++) {
        dir.registros[i] = no->registros[meio + i];
    }

    dir.proxima = no->proxima;           
    no->proxima = nova;
    no->num_chaves = meio;

    if (grava_no(pagina, no) != 0) return INSERE_ERRO;
    if (grava_no(nova, &dir) != 0) return INSERE_ERRO;

    *chave_promovida = dir.registros[0].id;
    *nova_pagina = nova;
    return INSERE_DIVIDIU;
}
```

#### Divisão de Interno

O nó original fica com as duas primeiras chaves. A terceira chave sobe para o nó pai. A quarta chave vai para um novo nó.

Retorna `INSERE_DIVIDIU` em caso de sucesso ou `INSERE_ERRO`.

``` C
int dividir_interno(int pagina, No *no, int32_t *chave_promovida, int *nova_pagina) {
    int nova = aloca_pagina();
    if (nova < 0) return INSERE_ERRO;

    int total = no->num_chaves;         
    int meio  = total / 2;               

    No dir;
    memset(&dir, 0, sizeof(dir));
    dir.tipo       = NO_INTERNO;
    dir.proxima    = SEM_PAGINA;
    dir.num_chaves = total - meio - 1;
    for (int i = 0; i < dir.num_chaves; i++) {
        dir.chaves[i] = no->chaves[meio + 1 + i];
    }
    for (int i = 0; i <= dir.num_chaves; i++) {
        dir.filhos[i] = no->filhos[meio + 1 + i];
    }

    *chave_promovida = no->chaves[meio];
    no->num_chaves = meio;

    if (grava_no(pagina, no) != 0) return INSERE_ERRO;
    if (grava_no(nova, &dir) != 0) return INSERE_ERRO;

    *nova_pagina = nova;
    return INSERE_DIVIDIU;
}
```

## Observações

1. A ordem 3 com páginas de 4KB **limita o uso de cada página**. Se a árvore possuir 20.000 chaves,a sua altura será de 10. Se houver 1 milhão,a estimativa é de 12 níveis. Se a ordem fosse de 255,a altura seria de apenas 3 níveis mesmo com 20.000 chaves.
2. Falta **atomicidade nas divisões**: Uma divisão grava esquerda → direita → pai (→ raiz → meta). Uma falha entre as gravações deixa a árvore inconsistente (folha nova existe, mas o pai não aponta para ela,gerando registros inalcançáveis por busca). Só o Log (WAL) pode resolver.
3. Falta **remoção e atualização**: No momento não há `DELETE` ou `UPDATE`. Se uma chave existente for inserida,um erro de duplicidade será retornado.
4. Tipagem **limitada**: Só há chaves `int32`. Falta suporte a chaves de texto,composta ou duplicadas.
5. Páginas alocadas **serão perdidas em caso de erro**: Se `grava_no` falha depois de `aloca_pagina`,a página fica vazia.
6. Não há **verificação de layout** em tempo de compilação: Se `MAX_CHAVES` for aumentado demais,os nó passam de 4KB sem houver aviso. Falta `static_assert` que limite as chaves em nó internos e folhas.
7. Falta **concorrência**: A árvore não é thread-safe e nem possuí locks.

## Conclusões

A Árvore B+ é uma estrutura de dados persistente e integrada ao chave que funciona como índice de chave primária para inserções e buscas.

É possível utilizá-la para realizar buscas por igualdade,inserções em ordem e persistência. Há dois modos possíveis de uso:

1. **Índice agrupado**: O `Registro` é a própria linha. Esse modo serve como armazenamento de tabela com duas colunas.
2. **Índice não agrupado** : Usa o `id` como chave indexada e o `valor` como identificador (posição da linha). 

No entanto,o Módulo 3 ainda não está pronto para ser usado como índice geral devido a ausência de recursos de remoção,atualização,varredura por faixa e recuperação após falhas.

Para que seja um índice completo são necessárias as seguintes funcionalidades:

1. **Iterador de faixa**: Habilita `BETWEEN,<,>,ORDER BY`
2. **Atualização de valor**: Habilita `UPDATE`
3. **Remoção com fusão e redistribuição**: Habilita `DELETE`
4. **Chaves duplicadas e compostas**
5. **Atomicidade de divisões**

# Módulo 4

## Introdução

O Módulo 4 se trata de um conjunto de arquivos responsáveis por receber comandos SQL e convertê-los em instruções para o CraftDB.

Em bancos de dados relacionais,o processamento de uma consulta passa por diversas etapas até encontrar os dados no disco. Os componentes **Tokenizador,Parser e Catálogo** são responsáveis por atuar nesse pipeline.

## Visão Geral

### Tokenizador

O Tokenizador é o componente que recebe um texto SQL e o converte em uma sequência de **tokens**. Ele ignora espaços em brancos,quebras de linhas e comentários,transformando um texto contínuo em uma sequência estruturada de palavra-chave,identificadores,operadores e literais.

``` SQL
CREATE TABLE alunos (id INT, nome CHAR);
INSERT INTO alunos VALUES (1, 'Ana');
SELECT * FROM alunos WHERE id >= 1 AND nome <> 'Bia';
```

Os tokens upper-case são palavras-chaves. Os tokens lower-case são identificadores. Os tokens `>`,`<` e  `=` são operadores. O token `1` é um literal numérico. O token `;` é um delimitador.

Em `Tokenizer.c` os tokens estão em um `enum` chamado `TipoToken`,o qual lista todas as categorias de tokens.

``` C
typedef enum {
    /* Palavras reservadas (comandos) */
    TOKEN_CREATE,
    TOKEN_TABLE,
    TOKEN_INSERT,
    TOKEN_INTO,
    TOKEN_VALUES,
    TOKEN_SELECT,
    TOKEN_FROM,
    TOKEN_WHERE,
    TOKEN_AND,

    /* Tipos de dados */
    TOKEN_INT,
    TOKEN_CHAR,

    /* Literais e identificadores */
    TOKEN_IDENTACAO,
    TOKEN_NUMERO,
    TOKEN_STRING,

    /* Separadores e símbolos */
    TOKEN_ESPACO,
    TOKEN_ABRE_PARENTESE,
    TOKEN_FECHA_PARENTESE,
    TOKEN_VIRGULA,
    TOKEN_PONTO_VIRGULA,
    TOKEN_ASTERISCO,

    /* Operadores de comparação (usados no WHERE) */
    TOKEN_IGUAL,
    TOKEN_DIFERENTE,
    TOKEN_MENOR,
    TOKEN_MENOR_IGUAL,
    TOKEN_MAIOR,
    TOKEN_MAIOR_IGUAL,

    /* Controle */
    TOKEN_NAO_SUPORTADO,   /* JOIN, ORDER, BY, GROUP, COUNT, OR, ...      */
    TOKEN_FIM,             /* final de texto                              */
    TOKEN_ERRO             /* caractere/lexema invalido                   */
} TipoToken;
```

Cada token é representado por uma `struct`,que guarda tipo,lexema,linha e coluna.

``` C
typedef struct {
    TipoToken tipo;
    char      lexema[MAX_LEXEMA];
    int       linha;
    int       coluna;
} Token;
``` 

O Tokenizador também é representado por uma `struct`,que guarda o texto de entrada e a posição atual.

``` C
typedef struct {
    const char *src;     //  texto de entrada 
    size_t      posicao;  //  posicao atual 
    int         linha;
    int         coluna;
} Tokenizador;
```

Os tokens são devolvidos exatamente na ordem em que aparecem no texto,cada um com uma linha e coluna de origem. Isso é feito porque no SQL a ordem muda o significado. Um comando `INSERT INTO ... VALUES` não é o mesmo que `VALUES ... INSERT INTO`. Sendo assim,o tokenizador garante que a sequência de saída seja fiél a sequência de entrada,pois o Parser deve validá-la depois.

Cada palavra reservada é representada por uma `struct`,que armazena a palavra e o tipo de token. As palavras reservadas são `CREATE,INSERT,INSERT,SELECT,FROM,WHERE,AND,INT e CHAR`.

``` C
typedef struct {
    const char *palavra;
    TipoToken   tipo;
} Reservada;
```

``` C
static const Reservada RESERVADAS[] = {
    /* suportadas */
    { "CREATE", TOKEN_CREATE }, { "TABLE",  TOKEN_TABLE  },
    { "INSERT", TOKEN_INSERT }, { "INTO",   TOKEN_INTO   },
    { "VALUES", TOKEN_VALUES }, { "SELECT", TOKEN_SELECT },
    { "FROM",   TOKEN_FROM   }, { "WHERE",  TOKEN_WHERE  },
    { "AND",    TOKEN_AND    },
    { "INT",    TOKEN_INT    }, { "CHAR",   TOKEN_CHAR   },

    /* fora do escopo: OR, juncao, ORDER BY, agregacao, GROUP BY */
    { "OR",     TOKEN_NAO_SUPORTADO },
    { "JOIN",   TOKEN_NAO_SUPORTADO }, { "INNER",  TOKEN_NAO_SUPORTADO },
    { "LEFT",   TOKEN_NAO_SUPORTADO }, { "RIGHT",  TOKEN_NAO_SUPORTADO },
    { "ON",     TOKEN_NAO_SUPORTADO },
    { "ORDER",  TOKEN_NAO_SUPORTADO }, { "BY",     TOKEN_NAO_SUPORTADO },
    { "GROUP",  TOKEN_NAO_SUPORTADO }, { "HAVING", TOKEN_NAO_SUPORTADO },
    { "COUNT",  TOKEN_NAO_SUPORTADO }, { "SUM",    TOKEN_NAO_SUPORTADO },
    { "AVG",    TOKEN_NAO_SUPORTADO }, { "MIN",    TOKEN_NAO_SUPORTADO },
    { "MAX",    TOKEN_NAO_SUPORTADO },
};
```

As palavras reservadas possuem prioridade sobre os identificadores. O código consulta primeiro a tabela `RESERVADAS` e caso a palavra não esteja lá,ela é classificada como um `TOKEN_NAO_SUPORTADO`.

O Tokenizador é responsável por devolver o próximo token que aparece na ordem do texto. Isso é feito por meio da função `proximo_token()`.

``` C
Token proximo_token(Tokenizador *t)
{
    Token token;
    size_t n = 0;
    int caractere;

    memset(&token, 0, sizeof token);
    token.linha  = t->linha;
    token.coluna = t->coluna;
    caractere = olhar(t);

    /* 1. Final de texto */
    if (caractere == '\0') {
        token.tipo = TOKEN_FIM;
        return token;
    }

    /* 2. Espaco (espaco, tab, \n, \r) - uma sequencia vira um token */
    if (isspace(caractere)) {
        while (isspace(olhar(t)))
            avancar(t);
        token.tipo = TOKEN_ESPACO;
        strcpy(token.lexema, " ");
        return token;
    }

    /* 3. Simbolos de um caractere */
    switch (caractere) {
        case '(': token.tipo = TOKEN_ABRE_PARENTESE;   break;
        case ')': token.tipo = TOKEN_FECHA_PARENTESE;  break;
        case ',': token.tipo = TOKEN_VIRGULA;          break;
        case ';': token.tipo = TOKEN_PONTO_VIRGULA;    break;
        case '*': token.tipo = TOKEN_ASTERISCO;        break;
        case '=': token.tipo = TOKEN_IGUAL;            break;
        default:  break;
    }
    if (caractere == '(' || caractere == ')' || caractere == ',' || caractere == ';' ||
        caractere == '*' || caractere == '=') {
        adicionar_caractere(&token, &n, avancar(t));
        return token;
    }

    /* 4. Operadores de 1 ou 2 caracteres: < <= <> > >= != */
    if (caractere == '<' || caractere == '>' || caractere == '!') {
        adicionar_caractere(&token, &n, avancar(t));
        int proximo = olhar(t);

        if (caractere == '<') {
            if (proximo == '=')      { adicionar_caractere(&token, &n, avancar(t)); token.tipo = TOKEN_MENOR_IGUAL; }
            else if (proximo == '>') { adicionar_caractere(&token, &n, avancar(t)); token.tipo = TOKEN_DIFERENTE;   }
            else                  { token.tipo = TOKEN_MENOR; }
        } else if (caractere == '>') {
            if (proximo == '=')      { adicionar_caractere(&token, &n, avancar(t)); token.tipo = TOKEN_MAIOR_IGUAL; }
            else                  { token.tipo = TOKEN_MAIOR; }
        } else { /* '!' so e valido como "!=" */
            if (proximo == '=')      { adicionar_caractere(&token, &n, avancar(t)); token.tipo = TOKEN_DIFERENTE; }
            else                  { token.tipo = TOKEN_ERRO; }
        }
        return token;
    }

    /* 5. Literal CHAR: 'texto' (no maximo TAM_REGISTRO caracteres) */
    if (caractere == '\'') {
        size_t tamanho = 0;
        avancar(t);                          /* abre aspas */
        while (olhar(t) != '\'' && olhar(t) != '\0') {
            adicionar_caractere(&token, &n, avancar(t));
            tamanho++;
        }
        if (olhar(t) != '\'') {              /* chegou ao fim sem fechar */
            token.tipo = TOKEN_ERRO;
            return token;
        }
        avancar(t);                          /* fecha aspas */
        token.tipo = (tamanho > TAM_REGISTRO) ? TOKEN_ERRO : TOKEN_STRING;
        return token;
    }

    /* 6. Literal INT: sequencia de digitos */
    if (isdigit(caractere)) {
        while (isdigit(olhar(t)))
            adicionar_caractere(&token, &n, avancar(t));
        /* numero colado em letra (ex.: 12abc) e invalido */
        if (isalpha(olhar(t)) || olhar(t) == '_') {
            while (isalnum(olhar(t)) || olhar(t) == '_')
                adicionar_caractere(&token, &n, avancar(t));
            token.tipo = TOKEN_ERRO;
        } else {
            token.tipo = TOKEN_NUMERO;
        }
        return token;
    }

    /* 7. Identificador ou palavra reservada */
    if (isalpha(caractere) || caractere == '_') {
        int estourou = 0;
        while (isalnum(olhar(t)) || olhar(t) == '_') {
            if (n >= MAX_LEXEMA - 1) estourou = 1;
            adicionar_caractere(&token, &n, avancar(t));
        }
        token.tipo = estourou ? TOKEN_ERRO : classificar_palavra(token.lexema);
        return token;
    }

    /* 8. Qualquer outro caractere e invalido */
    adicionar_caractere(&token, &n, avancar(t));
    token.tipo = TOKEN_ERRO;
    return token;
}
```
