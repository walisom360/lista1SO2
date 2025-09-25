// relay_race_fixed.c
// Corrida de revezamento com valores fixos (sem argumentos de linha de comando)

#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static inline long now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void ms_sleep_rand(int min_ms, int max_ms, unsigned *seed) {
  int span = max_ms - min_ms + 1;
  int ms = min_ms + (span > 0 ? (rand_r(seed) % span) : 0);
  struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
  nanosleep(&ts, NULL);
}

/* ---------- Barreira: pthread_barrier_t ---------- */
typedef pthread_barrier_t barrier_t;

static int barrier_init(barrier_t *b, unsigned count) {
  return pthread_barrier_init(b, NULL, count);
}
static int barrier_wait(barrier_t *b) {
  return pthread_barrier_wait(b);
}
static int barrier_destroy(barrier_t *b) {
  return pthread_barrier_destroy(b);
}
/* ------------------------------------------------- */

typedef struct {
  int                id;
  int                min_ms, max_ms;
  barrier_t         *barrier;
  atomic_long       *rounds;
  atomic_int        *running;
} worker_args_t;

static void *runner(void *arg) {
  worker_args_t *wa = (worker_args_t *)arg;
  unsigned seed = (unsigned)(time(NULL) ^ (wa->id * 2654435761u));
  while (atomic_load_explicit(wa->running, memory_order_relaxed)) {
    ms_sleep_rand(wa->min_ms, wa->max_ms, &seed);
    int rc = barrier_wait(wa->barrier);
    if (rc == PTHREAD_BARRIER_SERIAL_THREAD)
      atomic_fetch_add_explicit(wa->rounds, 1, memory_order_relaxed);
  }
  barrier_wait(wa->barrier);
  return NULL;
}

static double run_experiment(int K, double seconds, int min_ms, int max_ms) {
  pthread_t *th = calloc(K, sizeof(pthread_t));
  worker_args_t *args = calloc(K, sizeof(worker_args_t));
  barrier_t barrier;
  atomic_long rounds = 0;
  atomic_int running = 1;

  barrier_init(&barrier, (unsigned)K);

  for (int i = 0; i < K; i++) {
    args[i].id = i;
    args[i].min_ms = min_ms;
    args[i].max_ms = max_ms;
    args[i].barrier = &barrier;
    args[i].rounds = &rounds;
    args[i].running = &running;
    pthread_create(&th[i], NULL, runner, &args[i]);
  }

  long t0 = now_ns();
  long deadline = t0 + (long)(seconds * 1e9);
  while (now_ns() < deadline) {
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 50 * 1000000L };
    nanosleep(&ts, NULL);
  }

  atomic_store(&running, 0);

  for (int i = 0; i < K; i++) pthread_join(th[i], NULL);

  long total_rounds = atomic_load(&rounds);
  long t1 = now_ns();
  double elapsed_s = (double)(t1 - t0) / 1e9;

  barrier_destroy(&barrier);
  free(th);
  free(args);

  double rpm = (elapsed_s > 0.0) ? (total_rounds * 60.0 / elapsed_s) : 0.0;
  printf("Equipe K=%d → rodadas=%ld em %.2fs → RPM=%.2f\n", K, total_rounds, elapsed_s, rpm);
  return rpm;
}

int main(void) {
  // -------- Valores fixos --------
  double seconds = 10.0;      // duração em segundos
  int Ks[] = {2, 4, 8};       // tamanhos de equipe
  int nK = 3;
  int min_ms = 5, max_ms = 15; // trabalho simulado (ms)
  // -------------------------------

  printf("Duração por K: %.2fs | Trabalho aleatório: %d..%d ms\n", seconds, min_ms, max_ms);
  printf("------------------------------------------------------\n");
  for (int i = 0; i < nK; i++) {
    run_experiment(Ks[i], seconds, min_ms, max_ms);
  }
  printf("------------------------------------------------------\n");
  return 0;
}
