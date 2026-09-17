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

#### Módulo 3 - Árvore B+

#### Módulo 4 - Parser & Catálogo

#### Módulo 5 - Executor

#### Módulo 6 - Transações

#### Módulo 7 - Recuperação

## Autores

1.**Vinicius da Silva e Silva** - `202411140038`

2.**Kawe Alexander Sampaio da Silva** - `202411140041`

## Notas de Desenvolvimento

Este projeto foi desenvolvido com o auxílio dos seguintes modelos de IA Generativa:

1.**Anthropic Claude Sonnet 5**

2.**OpenAI ChatGPT 5.6 Luna**

3.**Google Gemini 3.5 Flash**

A sua utilização foi voltada aos seguintes campos:

1.**Prototipação**

2.**Refatoração**

3.**Testes**

4.**Análise**

5.**Sugestões**

Durante a criação e desenvolvimento do arquivo `NOTES.md`,não foram utilizados nenhum desses modelos de forma intensa.

A razão da utilização de LLMs se deve ao **cronograma de entrega** do projeto.
