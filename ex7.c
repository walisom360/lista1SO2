// dining.c
// Simulação dos Filósofos com duas estratégias anti-deadlock:
//  a) Ordem global de aquisição dos garfos
//  b) Semáforo (garçom) limitando a N-1 filósofos simultâneos
// Coleta métricas por filósofo e mitiga starvation via limite de sequência + backoff educado.

#define _GNU_SOURCE
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef enum { STRAT_ORDER = 1, STRAT_WAITER = 2 } strategy_t;

typedef struct {
    unsigned id;
    pthread_t tid;
    unsigned int rng; // seed por thread
    // métricas
    uint64_t meals;
    uint64_t total_wait_ns;
    uint64_t max_wait_ns;
    uint64_t consec_meals;
} phil_t;

static int N = 5;
static strategy_t STRATEGY = STRAT_ORDER;
static int RUN_SECONDS = 10;

// parâmetros de tempo (ms)
static int THINK_MIN_MS = 5, THINK_MAX_MS = 25;
static int EAT_MIN_MS   = 5, EAT_MAX_MS   = 15;

// mitigação de starvation
static uint64_t CONSEC_LIMIT = 3;

// recursos compartilhados
static pthread_mutex_t *forks;          // tamanho N
static sem_t waiter;                    // usado só na estratégia STRAT_WAITER
static pthread_mutex_t state_mx = PTHREAD_MUTEX_INITIALIZER;

static atomic_bool running = true;

// estado de fome para mitigação de starvation
static bool *hungry; // tamanho N

// utilidades de tempo
static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void sleep_ms(int ms) {
    if (ms <= 0) return;
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static int rand_in_range(unsigned int *seed, int lo, int hi) {
    // retorna inteiro uniforme entre [lo, hi]
    if (hi < lo) { int t = lo; lo = hi; hi = t; }
    int span = hi - lo + 1;
    return lo + (int)(rand_r(seed) % (unsigned)span);
}

static inline int left_fork(int id)  { return id; }
static inline int right_fork(int id) { return (id + 1) % N; }
static inline int left_neighbor(int id)  { return (id - 1 + N) % N; }
static inline int right_neighbor(int id) { return (id + 1) % N; }

// tentativa de pegar garfos conforme estratégia escolhida
static void take_forks_order(int id) {
    int L = left_fork(id);
    int R = right_fork(id);
    int first = (L < R) ? L : R;
    int second = (L < R) ? R : L;
    pthread_mutex_lock(&forks[first]);
    pthread_mutex_lock(&forks[second]);
}

static void put_forks_order(int id) {
    int L = left_fork(id);
    int R = right_fork(id);
    int first = (L < R) ? L : R;
    int second = (L < R) ? R : L;
    pthread_mutex_unlock(&forks[second]);
    pthread_mutex_unlock(&forks[first]);
}

static void take_forks_waiter(int id) {
    // Primeiro pede "permissão" ao garçom (N-1 simultâneos)
    sem_wait(&waiter);
    // Depois pega os dois garfos (pode ser L depois R)
    pthread_mutex_lock(&forks[left_fork(id)]);
    pthread_mutex_lock(&forks[right_fork(id)]);
}

static void put_forks_waiter(int id) {
    pthread_mutex_unlock(&forks[right_fork(id)]);
    pthread_mutex_unlock(&forks[left_fork(id)]);
    sem_post(&waiter);
}

// mitigação simples de starvation: se já comeu CONSEC_LIMIT vezes
// e algum vizinho está faminto, cede voluntariamente com um backoff.
static void fairness_yield_if_needed(phil_t *p) {
    if (CONSEC_LIMIT == 0) return;
    if (p->consec_meals < CONSEC_LIMIT) return;

    pthread_mutex_lock(&state_mx);
    bool leftH  = hungry[left_neighbor(p->id)];
    bool rightH = hungry[right_neighbor(p->id)];
    pthread_mutex_unlock(&state_mx);

    if (leftH || rightH) {
        // cede educadamente: zera a sequência e tira um cochilo curto
        p->consec_meals = 0;
        sleep_ms(rand_in_range(&p->rng, 1, 3));
    }
}

static void* philosopher_fn(void *arg) {
    phil_t *p = (phil_t*)arg;

    while (atomic_load(&running)) {
        // 1) Pensa
        int think_ms = rand_in_range(&p->rng, THINK_MIN_MS, THINK_MAX_MS);
        sleep_ms(think_ms);

        // 2) Fica com fome
        uint64_t t0 = now_ns();

        pthread_mutex_lock(&state_mx);
        hungry[p->id] = true;
        pthread_mutex_unlock(&state_mx);

        // 3) Estratégia contra deadlock
        if (STRATEGY == STRAT_ORDER) {
            take_forks_order(p->id);
        } else {
            take_forks_waiter(p->id);
        }

        // 4) Começou a comer: mede espera
        uint64_t waited = now_ns() - t0;
        p->total_wait_ns += waited;
        if (waited > p->max_wait_ns) p->max_wait_ns = waited;

        // 5) Come
        p->meals += 1;
        p->consec_meals += 1;

        int eat_ms = rand_in_range(&p->rng, EAT_MIN_MS, EAT_MAX_MS);
        sleep_ms(eat_ms);

        // 6) Larga os garfos
        if (STRATEGY == STRAT_ORDER) {
            put_forks_order(p->id);
        } else {
            put_forks_waiter(p->id);
        }

        pthread_mutex_lock(&state_mx);
        hungry[p->id] = false;
        pthread_mutex_unlock(&state_mx);

        // 7) Fairness: se estou monopolizando e vizinho quer comer, cedo
        fairness_yield_if_needed(p);
    }
    return NULL;
}

static void usage(const char *prog) {
    fprintf(stderr,
      "Uso: %s [--strategy order|waiter] [--seconds S] [--philosophers N]\n"
      "          [--think-ms a b] [--eat-ms a b] [--consec-limit K]\n"
      "Padrões: strategy=order, seconds=10, N=5, think=5..25ms, eat=5..15ms, K=3\n",
      prog);
}

static bool parse_args(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--strategy") && i+1 < argc) {
            if (!strcmp(argv[i+1], "order")) STRATEGY = STRAT_ORDER;
            else if (!strcmp(argv[i+1], "waiter")) STRATEGY = STRAT_WAITER;
            else { usage(argv[0]); return false; }
            i++;
        } else if (!strcmp(argv[i], "--seconds") && i+1 < argc) {
            RUN_SECONDS = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--philosophers") && i+1 < argc) {
            N = atoi(argv[++i]);
            if (N < 2) { fprintf(stderr, "N mínimo é 2\n"); return false; }
        } else if (!strcmp(argv[i], "--think-ms") && i+2 < argc) {
            THINK_MIN_MS = atoi(argv[++i]);
            THINK_MAX_MS = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--eat-ms") && i+2 < argc) {
            EAT_MIN_MS = atoi(argv[++i]);
            EAT_MAX_MS = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--consec-limit") && i+1 < argc) {
            CONSEC_LIMIT = strtoull(argv[++i], NULL, 10);
        } else {
            usage(argv[0]);
            return false;
        }
    }
    return true;
}

int main(int argc, char **argv) {
    if (!parse_args(argc, argv)) return 1;

    printf("Estratégia: %s | N=%d | dur=%ds | think=%d..%dms | eat=%d..%dms | consecLimit=%llu\n",
           (STRATEGY == STRAT_ORDER ? "ordem-global" : "garcom-4"),
           N, RUN_SECONDS, THINK_MIN_MS, THINK_MAX_MS, EAT_MIN_MS, EAT_MAX_MS,
           (unsigned long long)CONSEC_LIMIT);

    forks = calloc((size_t)N, sizeof(*forks));
    hungry = calloc((size_t)N, sizeof(*hungry));
    if (!forks || !hungry) {
        fprintf(stderr, "Falha de alocação\n");
        return 1;
    }

    for (int i = 0; i < N; ++i) pthread_mutex_init(&forks[i], NULL);
    if (STRATEGY == STRAT_WAITER) {
        // Permite no máximo N-1 filósofos "no salão" disputando garfos
        sem_init(&waiter, 0, N - 1);
    }

    phil_t *ph = calloc((size_t)N, sizeof(*ph));
    if (!ph) {
        fprintf(stderr, "Falha de alocação\n");
        return 1;
    }

    // cria threads
    unsigned int seed0 = (unsigned int)time(NULL);
    for (int i = 0; i < N; ++i) {
        ph[i].id = (unsigned)i;
        ph[i].rng = seed0 ^ (0x9E3779B9u * (unsigned)i);
        ph[i].meals = 0;
        ph[i].total_wait_ns = 0;
        ph[i].max_wait_ns = 0;
        ph[i].consec_meals = 0;
        if (pthread_create(&ph[i].tid, NULL, philosopher_fn, &ph[i]) != 0) {
            perror("pthread_create");
            return 1;
        }
    }

    // roda por RUN_SECONDS
    for (int s = 0; s < RUN_SECONDS; ++s) {
        sleep(1);
    }

    // sinaliza encerramento
    atomic_store(&running, false);

    // aguarda threads
    for (int i = 0; i < N; ++i) pthread_join(ph[i].tid, NULL);

    // métricas
    printf("\n== Métricas por filósofo ==\n");
    uint64_t total_meals = 0;
    uint64_t max_wait_ns_global = 0;
    uint64_t sum_wait_ns = 0;
    uint64_t waited_cnt = 0;

    for (int i = 0; i < N; ++i) {
        total_meals += ph[i].meals;
        if (ph[i].max_wait_ns > max_wait_ns_global) max_wait_ns_global = ph[i].max_wait_ns;
        sum_wait_ns += ph[i].total_wait_ns;
        waited_cnt += ph[i].meals;

        double avg_ms = (ph[i].meals > 0) ? ((double)ph[i].total_wait_ns / 1e6) / (double)ph[i].meals : 0.0;
        double max_ms = (double)ph[i].max_wait_ns / 1e6;

        printf("Filósofo %d: refeições=%llu | espera_média=%.3f ms | maior_espera=%.3f ms\n",
               i, (unsigned long long)ph[i].meals, avg_ms, max_ms);
    }

    double global_avg_ms = (waited_cnt > 0) ? ((double)sum_wait_ns / 1e6) / (double)waited_cnt : 0.0;
    printf("\nTotal de refeições: %llu\n", (unsigned long long)total_meals);
    printf("Espera média global: %.3f ms | Maior espera global: %.3f ms\n",
           global_avg_ms, (double)max_wait_ns_global / 1e6);

    // limpeza
    if (STRATEGY == STRAT_WAITER) sem_destroy(&waiter);
    for (int i = 0; i < N; ++i) pthread_mutex_destroy(&forks[i]);
    free(forks);
    free(hungry);
    free(ph);

    return 0;
}
