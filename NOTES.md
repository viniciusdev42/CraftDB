# Módulo 1

## Introdução

### O Módulo 1 se trata de um Sistema de Armazenamento em Páginas,o qual é responsável por alocar os bytes para unidades chamadas de Páginas,as quais são usadas pelos módulos superiores (Parser,Catálogo,Executor) para realizarem operações mais abstratas.

### O Módulo 1 inicialmente se tratava apenas páginas de 4KB,cabeçalhos de 16B e registros de 8B,possuindo somente funções de ler_pagina() e escrever_pagina().No entanto,à medida em que requisitos como Persistência e Isolamento foram sendo solicitados,a complexidade do módulo tornou-se cada vez maior.

### Essa seção chamada de Módulo 1 tem como objetivo discutir as funcionalidades implementadas e as decisões tomadas pela equipe,a fim de fornecer um pequeno relatório sobre o desenvolvimento do primeiro componente do projeto.

## Descrição

### Arquitetura Geral:

O sistema modela o arquivo `craft-db.dat` como um array de páginas de tamanho fixo.A página n sempre começa no byte `n * 4096` do arquivo.Dessa forma,é possível ter um acesso direto a qualquer página,usando apenas a **aritmética do endereço**.

Cada página tem 4096 bytes (4KB),os quais são divididos em:

1.**Cabeçalho**:Contém os primeiros 16 bytes,sendo reservados para os metadados.

2.**Registros**:É a área de 4080 bytes organizada em 510 slots de 8 bytes.

### Leitura e Escrita de Páginas:

Representam as duas funções centrais do sistema.Ambas calculam o offset da página (`n * TAMANHO_PAGINA`),posicionam o cursor do arquivo nesse ponto com `fseek`,e então transferem exatamente 4096 bytes com `fread`/`fwrite`.Não há necessidade de percorrer o arquivo, nem de manter índices auxiliares: a própria aritmética do offset *é* o índice.

A sua complexidade é de `O(1)` em relação ao número de páginas do banco,pois ler ou escrever na página 5 custa o mesmo do que escrever na página 5.000.000,pois `fseek` é uma operação de custo constante no sistema operacional.

### Persistência em Disco:

A operação de gravação em C passa por pelo menos duas camadas de buffer antes de chegar ao disco:o buffer da biblioteca padrão C (que está dentro do processo) e o cache de páginas do sistema operacional (que está fora do processo,mas ainda dentro da RAM).

O `fwrite` apenas garante que os dados saíram do buffer do C.O `fflush` os empurra para o SO.Já o `fsync` é o único que força o SO a efetivamente gravá-los no disco físico,realizando um bloqueio até que isso aconteça.

Em termos de custo computacional,a `fsync` é a operação mais cara de toda a biblioteca pois envolve acesso físico ao disco.

### Validação do Buffer e do Número da Página:

Como o C não carrega a informação do tamanho junto com ponteiros,as funções de leitura e escrita passaram a receber explicitamente o tamanho do buffer e validá-lo em relação ao valor de 4KB,além de verificar se o ponteiro é `NULL`.

O número da página é validado contra o intervalo `[0,total_paginas_alocadas()]`,dessa forma rejeitando páginas com valores negativos ou que ainda não foram alocadas.

### Sincronização de Dados:

Extrai a lógica de `fflush` + `fsync` em uma função pública e independente, que pode ser chamada a qualquer momento — por exemplo, como um "ponto de checkpoint" após uma sequência de operações, sem depender de que cada `escreve_pagina` já sincronize individualmente.

### Alocação de Página:

A função `total_paginas_alocadas()` calcula quantas páginas já existem dividindo o tamanho atual do arquivo (o qual é obtido com `fseek` + `ftell()`) por 4096.

A função `aloca_pagina()` usa esse valor para saber qual é o próximo número da página livre,escreve uma página zerada exatamente nessa posição (o que estende o arquivo) e devolve esse número.

A função `escreve_pagina()` exige que a página já esteja alocada,porém a função `aloca_pagina()` precisa escrever a nova página quando ela ainda não está alocada.Para resolver isso,foi necessário criar uma função chamada `escreve_pagina_buffer`,a qual faz a escrita sem fazer essa verificação.

### Isolamento entre Páginas:

Cada operação de leitura ou escrita passou a adquirir, via `fcntl(F_SETLKW)`, uma trava (`struct flock`) restrita exatamente ao **intervalo de bytes** da página envolvida (`[n*4096, n*4096+4095]`), liberando-a logo em seguida.Como a trava cobre só esse intervalo, uma escrita na página 7 nunca disputa, nem pode se sobrepor a, nenhuma trava relativa à página 8.

Esta foi, das oito funcionalidades,**a mais difícil de implementar**,não pela quantidade de código (já que possuí poucas linhas),mas pela natureza do problema,pois é a única funcionalidade cuja correção depende da *ordem de execução entre processos diferentes*,e não apenas da lógica sequencial dentro de uma única chamada. Além disso, travas POSIX via `fcntl` têm uma armadilha conhecida:elas são associadas ao par (processo, arquivo), não ao descritor específico, de modo que fechar *qualquer* descritor daquele arquivo no processo libera *todas* as travas que o processo detém sobre ele. Isso obrigou a manter rigorosamente o padrão "abrir → travar → operar → destravar → fechar" em uma única sequência ininterrupta, sem descritores duplicados abertos ao mesmo tempo. A correção também não é visualmente óbvia ao rodar o programa normalmente: um erro sutil no cálculo do intervalo da trava poderia deixar duas páginas se sobrepondo sem que isso gerasse nenhum sintoma evidente — por isso a verificação, nesse caso, exigiu inspecionar os bytes crus do arquivo (com `dd`/`od`), e não apenas confiar na saída do programa.

### Serialização e Desserialização:

Até esta etapa, um registro era tratado apenas como 8 bytes crus e opacos. Foi introduzido o tipo `Registro` (`id` + `valor`, dois inteiros de 4 bytes), junto com funções que convertem esse `struct` para exatamente 8 bytes em **little-endian** — e vice-versa — usando deslocamento de bits (`& 0xFF`, `>> 8`, `>> 16`, `>> 24`) byte a byte, em vez de depender de `memcpy` direto do `struct` ou de macros de endianness do sistema.

### Suíte de Testes

Encontra-se no diretório `Test/M1`.

Nela,temos um arquivo `m-1-test.c` independente,com o seu próprio `main`,que está ligado à bilbioteca `m-1.c`,a qual é usada por `m-1-main.c`.

Cada funcionalidade ganhou uma função de teste dedicada (alocação, validação, leitura/escrita, isolamento, serialização, sincronização, persistência), usando uma pequena infraestrutura própria (`verifica`/`VERIFICA`) que registra sucesso ou falha sem interromper a execução dos demais testes.

A maior parte dos testes é direta,tratando-se de configurar um estado,chamar a função,comparar o resultado esperado.
 
O teste de **persistência**, porém, teve uma complexidade própria:para provar de verdade que os dados sobrevivem ao fim da execução (e não apenas "parecem" persistir porque o mesmo processo ainda está de pé), foi necessário lançar, via `popen`, um **processo filho genuinamente novo**,o qual não possuí nenhum estado em memória herdado,relendo o arquivo do zero e devolvendo o resultado ao processo pai. Esse é o único teste do projeto cuja validação depende de orquestrar múltiplos processos, e não apenas de chamar funções dentro do mesmo programa.

## Conclusões

A complexidade **não cresceu de forma computacional**:todas as operações continuam O(1),porque o modelo de páginas de tamanho fixo com acesso por offset é, por construção, um modelo de custo constante. O que cresceu foi a complexidade **de raciocínio**:cada nova funcionalidade não adicionou apenas código, mas uma nova *garantia* que o restante do sistema passou a poder assumir como verdadeira — que o buffer tem o tamanho certo, que a página existe, que os dados realmente chegaram ao disco, que duas escritas concorrentes não se pisam, que o formato em disco não depende da máquina que o escreveu.

Essa progressão espelha, em miniatura, como sistemas de armazenamento reais evoluem,indo de um mecanismo bruto de leitura/escrita, para uma camada com validação defensiva, depois para uma camada com garantias de durabilidade, depois para gerenciamento de espaço (alocação), depois para segurança sob concorrência, e só então para um formato de dados portátil e testável.

