// bank.c (POSIX/pthreads)
// Simula M contas e T threads com transferências aleatórias.
// SAFE (default): corretude com trava por conta (ou por partição com -p).
// UNSAFE (-u): sem trava, para evidenciar corrida. Relatório final mostra APENAS saldos.
// Compilar: gcc -O2 -Wall -Wextra -pthread -o bank bank.c

#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <getopt.h>

typedef struct {
    long long balance;
    pthread_mutex_t lock; // usado no modo "por conta"
} Account;

static Account *accounts = NULL;
static int M = 100;           // contas
static int Tthr = 4;          // threads
static int OPS = 100000;      // ops por thread
static long long INIT_BAL = 1000;
static unsigned long long SEED = 0;
static int CHECK_EVERY = 0;   // checar soma a cada K ops (safe)
static bool UNSAFE = false;   // sem travas
static int P = 0;             // partições (0 = mutex por conta)
static pthread_mutex_t *part_lock = NULL; // locks por partição
static bool LIST_BALANCES = false; // -l: listar saldos por conta ao final

// ---------- RNG (xorshift64*) ----------
static unsigned long long xs64(unsigned long long *s) {
    unsigned long long x = *s ? *s : 0x9E3779B97F4A7C15ULL;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    *s = x;
    return x * 2685821657736338717ULL;
}
static inline unsigned urand(unsigned long long *s) { return (unsigned)(xs64(s) >> 32); }
static inline int rnd_between(unsigned long long *s, int lo, int hi) {
    if (hi < lo) { int t = lo; lo = hi; hi = t; }
    unsigned span = (unsigned)(hi - lo + 1);
    return lo + (int)(urand(s) % span);
}

// ---------- Helpers ----------
static inline unsigned long long now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec*1000ULL + (unsigned long long)ts.tv_nsec/1000000ULL;
}

// Snapshot SOMA (com locks corretos para consistência no modo safe)
static long long sum_all_locked(void) {
    if (P > 0) {
        for (int i = 0; i < P; ++i) pthread_mutex_lock(&part_lock[i]);
        long long s = 0;
        for (int i = 0; i < M; ++i) s += accounts[i].balance;
        for (int i = P-1; i >= 0; --i) pthread_mutex_unlock(&part_lock[i]);
        return s;
    } else {
        for (int i = 0; i < M; ++i) pthread_mutex_lock(&accounts[i].lock);
        long long s = 0;
        for (int i = 0; i < M; ++i) s += accounts[i].balance;
        for (int i = M-1; i >= 0; --i) pthread_mutex_unlock(&accounts[i].lock);
        return s;
    }
}

// Modo inseguro: soma sem locks (apenas para mostrar saldo total)
static long long sum_all_unlocked(void) {
    long long s = 0;
    for (int i = 0; i < M; ++i) s += accounts[i].balance;
    return s;
}

// ---------- Transferências ----------
static void transfer_safe_accountlocks(int src, int dst, unsigned long long *rng) {
    int a = (src < dst) ? src : dst;
    int b = (src < dst) ? dst : src;
    pthread_mutex_lock(&accounts[a].lock);
    pthread_mutex_lock(&accounts[b].lock);

    long long sb = accounts[src].balance;
    if (sb > 0) {
        long long max_amt = sb / 10; if (max_amt < 1) max_amt = 1;
        long long amt = (long long)rnd_between(rng, 1, (int)max_amt);
        accounts[src].balance -= amt;
        accounts[dst].balance += amt;
        assert(accounts[src].balance >= 0);
        assert(accounts[dst].balance >= 0);
    }

    pthread_mutex_unlock(&accounts[b].lock);
    pthread_mutex_unlock(&accounts[a].lock);
}

static void transfer_safe_partitionlocks(int src, int dst, unsigned long long *rng) {
    int ps = src % P, pd = dst % P;
    if (ps == pd) {
        pthread_mutex_lock(&part_lock[ps]);
    } else if (ps < pd) {
        pthread_mutex_lock(&part_lock[ps]);
        pthread_mutex_lock(&part_lock[pd]);
    } else {
        pthread_mutex_lock(&part_lock[pd]);
        pthread_mutex_lock(&part_lock[ps]);
    }

    long long sb = accounts[src].balance;
    if (sb > 0) {
        long long max_amt = sb / 10; if (max_amt < 1) max_amt = 1;
        long long amt = (long long)rnd_between(rng, 1, (int)max_amt);
        accounts[src].balance -= amt;
        accounts[dst].balance += amt;
        assert(accounts[src].balance >= 0);
        assert(accounts[dst].balance >= 0);
    }

    if (ps == pd) {
        pthread_mutex_unlock(&part_lock[ps]);
    } else {
        pthread_mutex_unlock(&part_lock[ps < pd ? pd : ps]);
        pthread_mutex_unlock(&part_lock[ps < pd ? ps : pd]);
    }
}

static void transfer_unsafe(int src, int dst, unsigned long long *rng) {
    // Sem locks (proposital): entre ler e escrever pode ocorrer interferência
    long long sb = accounts[src].balance;
    if (sb > 0) {
        long long max_amt = sb / 10; if (max_amt < 1) max_amt = 1;
        long long amt = (long long)rnd_between(rng, 1, (int)max_amt);
        sched_yield(); // aumenta chance de corrida
        accounts[src].balance = sb - amt;
        accounts[dst].balance += amt;
    }
}

// ---------- Worker ----------
typedef struct {
    int id;
    unsigned long long rng;
} WorkerArg;

static void* worker_fn(void *arg) {
    WorkerArg *wa = (WorkerArg*)arg;
    for (int k = 0; k < OPS; ++k) {
        int src = rnd_between(&wa->rng, 0, M-1);
        int dst = rnd_between(&wa->rng, 0, M-2); if (dst >= src) dst++;

        if (!UNSAFE) {
            if (P > 0) transfer_safe_partitionlocks(src, dst, &wa->rng);
            else       transfer_safe_accountlocks(src, dst, &wa->rng);

            if (CHECK_EVERY > 0 && ((k+1) % CHECK_EVERY) == 0) {
                long long s = sum_all_locked();
                assert(s == (long long)M * INIT_BAL);
            }
        } else {
            transfer_unsafe(src, dst, &wa->rng);
        }
    }
    return NULL;
}

// ---------- CLI ----------
static void usage(const char *p) {
    fprintf(stderr,
        "Uso: %s [-m Mcontas] [-t Tthreads] [-o OPSporThread] [-b saldoInicial]\n"
        "          [-s seed] [-c checkEvery] [-p Pparticoes] [-u] [-l]\n"
        "  -m M   numero de contas (default 100)\n"
        "  -t T   numero de threads (default 4)\n"
        "  -o OPS operacoes por thread (default 100000)\n"
        "  -b BAL saldo inicial por conta (default 1000)\n"
        "  -s S   semente RNG (default time-based)\n"
        "  -c K   checar soma a cada K ops (apenas SAFE; default 0=so final)\n"
        "  -p P   travas por particao (P>0). P=0 => mutex por conta (default)\n"
        "  -u     modo INSEGURO (sem travas)\n"
        "  -l     listar saldos por conta no final\n", p);
}

int main(int argc, char **argv) {
    int opt;
    while ((opt = getopt(argc, argv, "m:t:o:b:s:c:p:ulh")) != -1) {
        switch (opt) {
            case 'm': M = atoi(optarg); break;
            case 't': Tthr = atoi(optarg); break;
            case 'o': OPS = atoi(optarg); break;
            case 'b': INIT_BAL = atoll(optarg); break;
            case 's': SEED = strtoull(optarg, NULL, 10); break;
            case 'c': CHECK_EVERY = atoi(optarg); break;
            case 'p': P = atoi(optarg); break;
            case 'u': UNSAFE = true; break;
            case 'l': LIST_BALANCES = true; break;
            default: usage(argv[0]); return 1;
        }
    }
    if (M <= 1 || Tthr <= 0 || OPS <= 0 || INIT_BAL < 0 || P < 0) {
        usage(argv[0]); return 1;
    }

    printf("Config: M=%d, T=%d, OPS/thread=%d, init=%lld, mode=%s, seed=%s, checkEvery=%d, partitions=%d\n",
           M, Tthr, OPS, INIT_BAL, (UNSAFE?"UNSAFE":"SAFE"),
           (SEED? "fixed" : "time"), CHECK_EVERY, P);

    accounts = (Account*)calloc((size_t)M, sizeof(Account));
    if (!accounts) { fprintf(stderr, "alloc accounts failed\n"); return 1; }
    for (int i = 0; i < M; ++i) {
        accounts[i].balance = INIT_BAL;
        pthread_mutex_init(&accounts[i].lock, NULL);
    }
    if (P > 0) {
        part_lock = (pthread_mutex_t*)malloc(sizeof(pthread_mutex_t)*(size_t)P);
        if (!part_lock) { fprintf(stderr, "alloc part_lock failed\n"); return 1; }
        for (int i = 0; i < P; ++i) pthread_mutex_init(&part_lock[i], NULL);
    }

    const long long INITIAL_TOTAL = (long long)M * INIT_BAL;

    pthread_t *th = (pthread_t*)malloc(sizeof(pthread_t)*(size_t)Tthr);
    WorkerArg *wa = (WorkerArg*)calloc((size_t)Tthr, sizeof(WorkerArg));
    if (!th || !wa) { fprintf(stderr, "alloc threads failed\n"); return 1; }

    unsigned long long base_seed = SEED ? SEED : (unsigned long long)time(NULL);
    unsigned long long t0 = now_ms();

    for (int i = 0; i < Tthr; ++i) {
        wa[i].id = i;
        wa[i].rng = base_seed ^ (0x9E3779B97F4A7C15ULL * (unsigned long long)i);
        if (pthread_create(&th[i], NULL, worker_fn, &wa[i]) != 0) {
            fprintf(stderr, "pthread_create: %s\n", strerror(errno));
            return 2;
        }
    }

    for (int i = 0; i < Tthr; ++i) pthread_join(th[i], NULL);
    unsigned long long t1 = now_ms();

    // Importante: antes de destruir locks, tire o snapshot do saldo total
    long long final_sum = UNSAFE ? sum_all_unlocked() : sum_all_locked();

    // Relatório final APENAS com saldos (como pedido)
    printf("Tempo: %llums\n", (unsigned long long)(t1 - t0));
    printf("Saldo total final: %lld\n", final_sum);
    if (LIST_BALANCES) {
        for (int i = 0; i < M; ++i) {
            printf("Conta %d: %lld\n", i, accounts[i].balance);
        }
    }

    // Prova por asserção (modo seguro) — não imprime delta
    if (!UNSAFE) {
        assert(final_sum == INITIAL_TOTAL);
    }

    // limpeza
    for (int i = 0; i < M; ++i) pthread_mutex_destroy(&accounts[i].lock);
    if (P > 0) { for (int i = 0; i < P; ++i) pthread_mutex_destroy(&part_lock[i]); }
    free(part_lock);
    free(wa);
    free(th);
    free(accounts);

    return 0;
}
