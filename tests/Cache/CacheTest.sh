#!/bin/bash
# Compila e executa a suite de testes de aceitacao do cache de paginas (Cache.c.c).
# Uso: ./executa_testes.sh
# Saida: 0 se todos os testes passarem; 1 se algum falhar.

set -u
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$DIR"

echo "Compilando os testes de aceitacao..."
gcc -std=c11 -Wall -Wextra -o CacheTest1 CacheTest1.c || exit 2
gcc -std=c11 -Wall -Wextra -o CacheTest2 CacheTest2.c|| exit 2
gcc -std=c11 -Wall -Wextra -o CacheTest3 CacheTest3.c || exit 2

FALHAS=0

echo
echo ">>> Teste 1: 4 frames - multiplas leituras da mesma pagina = uma unica leitura de disco"
rm -f craft-db.dat
./CacheTest1
if [ $? -ne 0 ]; then FALHAS=$((FALHAS + 1)); fi

echo
echo ">>> Teste 2: 3 frames - ao tocar 4 paginas, a menos usada e expulsa (LRU)"
rm -f craft-db.dat
./CacheTest2
if [ $? -ne 0 ]; then FALHAS=$((FALHAS + 1)); fi

echo
echo ">>> Teste 3: alterar + expulsar + reabrir em outro processo => alteracao persiste"
rm -f craft-db.dat
./CacheTest3 escritor
if [ $? -ne 0 ]; then FALHAS=$((FALHAS + 1)); fi
./CacheTest3 leitor
if [ $? -ne 0 ]; then FALHAS=$((FALHAS + 1)); fi

echo
if [ "$FALHAS" -eq 0 ]; then
    echo "=================================================="
    echo " TODOS OS TESTES DE ACEITACAO PASSARAM."
    echo "=================================================="
    exit 0
else
    echo "=================================================="
    echo " $FALHAS verificacao(oes)/etapa(s) de teste falharam."
    echo "=================================================="
    exit 1
fi
