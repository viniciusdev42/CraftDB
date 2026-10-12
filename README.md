# CraftDB

## Introdução

O **CraftDB** é um banco de dados desenvolvido em `C` a fim de compreender como Páginas,Árvores B+,Parsers,Executores,Transações e Logs funcionam a partir do baixo nível.

O intuito desse projeto não é desenvolver uma alternativa ao `SQLite`,mas sim criar algo simples capaz de ser entendido e executado por um sistema com especificações modestas.

## Descrição

O CraftDB é desenvolvido a partir de 7 módulos,cada um representando uma **funcionalidade implementada**.Cada implementação é feita a partir do módulo anterior.

#### Módulo 1 - Páginas

Trata-se de um **Sistema de Armazenamento de Páginas**,o qual é responsável por alocar bytes em unidades chamadas de Páginas,as quais estão armazenadas de forma física no disco.

É o módulo de mais baixo nível,sendo a base para o desenvolvimento e o funcionamento dos módulos seguintes.

#### Módulo 2 - Cache de Páginas

Trata-se de um mecanismo de **Cache**,o qual é responsável por acelerar as leituras realizadas pelo CraftDB às páginas.

Ele adota as políticas **STEAL** e **NO-FORCE**,junto do algoritmo **LRU**.Dessa forma,as páginas sujas podem ser gravadas em disco antes de um `COMMIT`.

Esse módulo servirá de suporte aos `Logs` do **Módulo 6**.

#### Módulo 3 - Árvore B+

Trata-se de uma estrutura de dados que armazena registros do tipo `int32`. Nela,um nó é uma página de 4KB,o qual armazena o tipo (`INTERNO(0) OU FOLHA(1)`),o número de chaves e um ponteiro para o próximo nó.

Na Árvore B+ o número de chaves por nó é 3. Escolheu-se esse valor para fins didáticos pois com ele é fácil observar as divisões de nó. Além disso,com uma ordem 3,cerca de 99% da página fica vazia ao inserir 20.000 chaves,o que mantém a Árvore B+ com uma altura de 10.

#### Módulo 4 - Parser & Catálogo

#### Módulo 5 - Executor

#### Módulo 6 - Transações

#### Módulo 7 - Recuperação

## Autores

1.**Vinicius da Silva e Silva** - `202411140038`

2.**Kawe Alexander Sampaio da Silva** - `202411140041`

## Notas de Desenvolvimento

Este projeto foi desenvolvido com o auxílio dos seguintes modelos de IA Generativa:

1.**Anthropic Claude Sonnet 5.5**

2.**OpenAI ChatGPT 5.6 Luna**

3.**Google Gemini 3.5 Flash Lite**

A sua utilização foi voltada aos seguintes campos:

1.**Prototipação**

2.**Refatoração**

3.**Testes**

4.**Análise**

5.**Sugestões**

Durante a criação e desenvolvimento do arquivo `NOTES.md`,não foram utilizados nenhum desses modelos de forma intensa.

A razão da utilização de LLMs se deve ao **cronograma de entrega** do projeto.
