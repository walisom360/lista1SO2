# Questão 1

## Descrição

- Cada cavalo é uma **thread** que avança em passos aleatórios.
- O usuário faz uma **aposta** antes da largada.
- A largada ocorre de forma **sincronizada** para todos os cavalos.
- O **placar** é atualizado com exclusão mútua (sem condições de corrida).
- O **vencedor** é definido de forma determinística, mesmo em caso de empate.
- Ao final, o programa informa se a **aposta** do usuário foi correta.

---

## Decisões

- Mutexes

- score_mutex → protege atualização do placar.
- winner_mutex → garante decisão atômica do vencedor.
- print_mutex → evita embaralhar mensagens no terminal.

- Barreiras (mutex + condvar)

- start_barrier → garante largada sincronizada.
- round_barrier → sincroniza cada rodada entre cavalos.

- Flag

- Protegida por mutex, usada para encerrar todas as threads de forma limpa.

## Como compilar e executar

Compilação:

```bash
gcc -O2 -pthread -o ex1 ex1.c
```

# Questão 2

## Descrição

**buffer circular** de tamanho `N` acessado por **múltiplos produtores** e **múltiplos consumidores**.

- **Semáforos** garantem contagem de **vagas** e **itens** (espera ativa zero).
- **Mutex** protege a **região crítica** do buffer (push/pop).
- Itens têm **timestamp** de enfileiramento para medir **latência de buffer**.
- Medimos **throughput (itens/s)**, **tempo médio de espera de produtores/consumidores** e **latência média no buffer**.
- Consumidores finalizam via **“pílulas venenosas”** (itens sentinela com `id = -1`).

---

## Decisões

- Semáforos

- sem_empty: conta vagas no buffer (produtor faz sem_wait antes de inserir).
  sem_full: conta itens no buffer (consumidor faz sem_wait antes de retirar).

- Mutexes

- buf_mtx → protege push/pop no buffer circular (região crítica).
  metrics_mtx → acumulação de métricas (ns) sem condição de corrida.

- Espera ativa zero

- Todos os bloqueios de disponibilidade (vagas/itens) usam sem_wait (sem busy-wait).
  Acesso ao buffer sempre com mutex.

- Encerramento limpo

- Após todos os produtores terminarem, a main injeta C pílulas buffer para acordar e finalizar cada consumidor.

## Como compilar

```bash
gcc -O2 -pthread -o ex2 ex2.c
```
