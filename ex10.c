// deadlock_watchdog.c
// Cenário de deadlock proposital + watchdog + correção com ordem total.
// Compile: gcc -O2 -pthread deadlock_watchdog.c -o deadlock
// Execute: ./deadlock
// Opcional: ./deadlock -r 5 -w 5 -t 3 -d 15

#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

// ---------- helpers de tempo ----------
static inline long long now_ns(void){
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec*1000000000LL + ts.tv_nsec;
}
static inline void sleep_ms(int ms){
  struct timespec ts = { .tv_sec = ms/1000, .tv_nsec = (long)(ms%1000)*1000000L };
  nanosleep(&ts, NULL);
}

// ---------- configuração com defaults seguros p/ “online” ----------
typedef struct {
  int R;                 // recursos (mutexes)
  int W;                 // workers
  int watchdog_timeout;  // s sem progresso => suspeita
  int duration;          // s por fase
} config_t;

static void parse_args(int argc, char**argv, config_t *cfg){
  // defaults (bons para demo online)
  cfg->R = 5;
  cfg->W = 5;
  cfg->watchdog_timeout = 3;
  cfg->duration = 12;

  int opt;
  while ((opt = getopt(argc, argv, "r:w:t:d:h")) != -1){
    switch (opt){
      case 'r': cfg->R = atoi(optarg); break;
      case 'w': cfg->W = atoi(optarg); break;
      case 't': cfg->watchdog_timeout = atoi(optarg); break;
      case 'd': cfg->duration = atoi(optarg); break;
      default:
        fprintf(stderr,
          "Uso: %s [-r recursos] [-w workers] [-t timeout_watchdog_s] [-d duracao_s]\n", argv[0]);
        exit(1);
    }
  }
  if (cfg->R < 2) cfg->R = 2;
  if (cfg->W < 2) cfg->W = 2;
  if (cfg->watchdog_timeout < 1) cfg->watchdog_timeout = 1;
  if (cfg->duration < 3) cfg->duration = 3;
}

// ---------- estado por thread (para o relatório do watchdog) ----------
typedef struct {
  atomic_int holding_any;     // 1 se segura ao menos 1 lock
  atomic_int hold_a;          // idx do 1º lock segurado (ou -1)
  atomic_int hold_b;          // idx do 2º lock segurado (ou -1)
  atomic_int waiting_for;     // idx do lock que está tentando adquirir (ou -1)
  atomic_long last_progress_ns; // timestamp do último progresso
  atomic_long ops;            // quantas operações concluiu
} thread_state_t;

// ---------- contexto da fase ----------
typedef struct {
  config_t cfg;
  pthread_mutex_t *locks; // recursos
  pthread_t *threads;
  thread_state_t *states;
  atomic_long total_ops;
  atomic_int stop;
  int safe_mode; // 0 = inseguro (pode deadlock), 1 = ordenado (evita deadlock)
  pthread_t watchdog_th;
} phase_t;

// ---------- aquisição anotada (para o watchdog saber intenções) ----------
static void acquire_lock_annotated(phase_t *ph, int tid, int rid){
  thread_state_t *st = &ph->states[tid];
  atomic_store(&st->waiting_for, rid);
  int rc = pthread_mutex_lock(&ph->locks[rid]);
  (void)rc;
  // marcar que está segurando
  if (atomic_load(&st->hold_a) == -1) atomic_store(&st->hold_a, rid);
  else atomic_store(&st->hold_b, rid);
  atomic_store(&st->waiting_for, -1);
  atomic_store(&st->holding_any, 1);
}
static void release_lock_annotated(phase_t *ph, int tid, int rid){
  pthread_mutex_unlock(&ph->locks[rid]);
  thread_state_t *st = &ph->states[tid];
  int a = atomic_load(&st->hold_a);
  int b = atomic_load(&st->hold_b);
  if (a == rid) atomic_store(&st->hold_a, -1);
  if (b == rid) atomic_store(&st->hold_b, -1);
  int na = atomic_load(&st->hold_a);
  int nb = atomic_load(&st->hold_b);
  if (na == -1 && nb == -1) atomic_store(&st->holding_any, 0);
}

// ---------- worker ----------
static void *worker(void *arg){
  long tid = (long)arg;
  extern phase_t g_phase;
  phase_t *ph = &g_phase;
  thread_state_t *st = &ph->states[tid];
  unsigned seed = (unsigned)(time(NULL) ^ (tid*2654435761u));
  int R = ph->cfg.R;

  while (!atomic_load(&ph->stop)){
    // Escolher dois recursos (formar conflitos intencionais).
    int a = (int)(tid % R);
    int b = (int)((tid+1) % R);

    int first = a, second = b;
    if (!ph->safe_mode){
      // Modo inseguro: às vezes invertemos a ordem para criar espera circular.
      int invert = ((tid % 2)==1); // ímpares bloqueiam na ordem oposta
      if (invert){ first = b; second = a; }
    } else {
      // Modo seguro: sempre menor -> maior (ordem total)
      if (second < first){ int t=first; first=second; second=t; }
    }

    // trabalho antes do lock
    sleep_ms(1 + (rand_r(&seed)%3)); // jitter leve

    // pegar locks na ordem definida
    acquire_lock_annotated(ph, (int)tid, first);
    sleep_ms(1 + (rand_r(&seed)%2)); // aumentar janela de interleaving
    acquire_lock_annotated(ph, (int)tid, second);

    // seção crítica simulada
    sleep_ms(1 + (rand_r(&seed)%2));

    // progresso
    atomic_fetch_add(&ph->total_ops, 1);
    atomic_fetch_add(&st->ops, 1);
    atomic_store(&st->last_progress_ns, now_ns());

    // soltar
    release_lock_annotated(ph, (int)tid, second);
    release_lock_annotated(ph, (int)tid, first);

    // descanso
    sleep_ms(1 + (rand_r(&seed)%2));
  }

  return NULL;
}

// ---------- watchdog ----------
static void *watchdog(void *arg){
  (void)arg;
  extern phase_t g_phase;
  phase_t *ph = &g_phase;
  long long last_ops = atomic_load(&ph->total_ops);
  long long last_change = now_ns();
  const long long timeout_ns = (long long)ph->cfg.watchdog_timeout * 1000000000LL;

  while (!atomic_load(&ph->stop)){
    sleep_ms(200);
    long long cur_ops = atomic_load(&ph->total_ops);
    if (cur_ops != last_ops){
      last_ops = cur_ops;
      last_change = now_ns();
      continue;
    }
    long long idle = now_ns() - last_change;
    if (idle >= timeout_ns){
      // Sem progresso por T s → suspeita de deadlock
      fprintf(stderr, "\n[WATCHDOG] Sem progresso por %d s. Possível deadlock.\n", ph->cfg.watchdog_timeout);
      fprintf(stderr, "[WATCHDOG] Snapshot de estados (tid: hold_a,hold_b | esperando):\n");
      for (int i=0;i<ph->cfg.W;i++){
        thread_state_t *st = &ph->states[i];
        int ha = atomic_load(&st->hold_a);
        int hb = atomic_load(&st->hold_b);
        int wf = atomic_load(&st->waiting_for);
        long long lp = atomic_load(&st->last_progress_ns);
        double secs = (now_ns()-lp)/1e9;
        fprintf(stderr, "  T%02d: hold=(%d,%d) wait=%d last_prog=%.2fs ops=%ld\n",
                i, ha, hb, wf, secs, (long)atomic_load(&st->ops));
      }
      // encerra fase
      atomic_store(&ph->stop, 1);
      break;
    }
  }
  return NULL;
}

// ---------- recursos globais (para referência cruzada rápida) ----------
phase_t g_phase;

// ---------- inicialização/limpeza ----------
static void phase_init(phase_t *ph, config_t cfg, int safe_mode){
  memset(ph, 0, sizeof(*ph));
  ph->cfg = cfg;
  ph->safe_mode = safe_mode;
  ph->locks = calloc(cfg.R, sizeof(pthread_mutex_t));
  ph->threads = calloc(cfg.W, sizeof(pthread_t));
  ph->states = calloc(cfg.W, sizeof(thread_state_t));
  atomic_store(&ph->total_ops, 0);
  atomic_store(&ph->stop, 0);
  for (int i=0;i<cfg.R;i++){
    pthread_mutex_init(&ph->locks[i], NULL);
  }
  for (int i=0;i<cfg.W;i++){
    atomic_store(&ph->states[i].holding_any, 0);
    atomic_store(&ph->states[i].hold_a, -1);
    atomic_store(&ph->states[i].hold_b, -1);
    atomic_store(&ph->states[i].waiting_for, -1);
    atomic_store(&ph->states[i].last_progress_ns, now_ns());
    atomic_store(&ph->states[i].ops, 0);
  }
}
static void phase_destroy(phase_t *ph){
  for (int i=0;i<ph->cfg.R;i++){
    pthread_mutex_destroy(&ph->locks[i]);
  }
  free(ph->locks); ph->locks=NULL;
  free(ph->threads); ph->threads=NULL;
  free(ph->states); ph->states=NULL;
}

// ---------- executar uma fase ----------
static void run_phase(const char *label){
  phase_t *ph = &g_phase;
  printf("=== %s === (R=%d, W=%d, watchdog=%ds, duracao=%ds)\n",
         label, ph->cfg.R, ph->cfg.W, ph->cfg.watchdog_timeout, ph->cfg.duration);

  // spawn workers
  for (long i=0;i<ph->cfg.W;i++){
    pthread_create(&ph->threads[i], NULL, worker, (void*)i);
  }
  // spawn watchdog
  pthread_create(&ph->watchdog_th, NULL, watchdog, NULL);

  long long t0 = now_ns();
  long long deadline = t0 + (long long)ph->cfg.duration * 1000000000LL;
  while (now_ns() < deadline && !atomic_load(&ph->stop)){
    sleep_ms(50);
  }
  atomic_store(&ph->stop, 1);

  // join
  for (int i=0;i<ph->cfg.W;i++) pthread_join(ph->threads[i], NULL);
  pthread_join(ph->watchdog_th, NULL);

  long long ops = atomic_load(&ph->total_ops);
  double elapsed = (now_ns()-t0)/1e9;
  double ops_s = (elapsed>0)? ops/elapsed : 0.0;
  printf("%s: ops=%lld em %.2fs (%.1f ops/s)\n\n", label, ops, elapsed, ops_s);
}

// ---------- main: roda Fase A (insegura) e Fase B (ordenada) ----------
int main(int argc, char **argv){
  config_t cfg; parse_args(argc, argv, &cfg);

  // Fase A: insegura (propensa a deadlock)
  phase_init(&g_phase, cfg, /*safe_mode=*/0);
  run_phase("FASE A (insegura: ordem variável de locks)"); // watchdog pode interromper
  phase_destroy(&g_phase);

  // Fase B: segura (ordem total)
  phase_init(&g_phase, cfg, /*safe_mode=*/1);
  run_phase("FASE B (segura: ordem total de travamento)");
  phase_destroy(&g_phase);

  printf("Concluído.\n");
  return 0;
}
