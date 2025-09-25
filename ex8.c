// ex2_extended.c
// Buffer circular P/C com bursts, backpressure (HWM/LWM) e logging de ocupação.
//
// Compilar:   gcc -std=c11 -O2 -pthread ex2_extended.c -o ex2_ext
// Executar:   ./ex2_ext
// (Opcional)  ./ex2_ext -p 4 -c 2 -n 128 -d 15 -b 50 -i 300 -w 96:64 -s 50

#define _GNU_SOURCE
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

typedef struct {
  long id;
  long long enq_ns; // timestamp de enfileiramento
} item_t;

// ---------- tempo ----------
static inline long long now_ns(void){
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec*1000000000LL + ts.tv_nsec;
}
static inline void sleep_ms(int ms){
  struct timespec ts = { .tv_sec = ms/1000, .tv_nsec = (long)(ms%1000)*1000000L };
  nanosleep(&ts, NULL);
}

// ---------- parâmetros ----------
typedef struct {
  int P, C, N;
  int duration_s;
  int burst_size;
  int idle_ms;
  int hwm, lwm;      // high/low watermark
  int sample_ms;     // período da amostragem da ocupação
} config_t;

static void set_defaults(config_t *cfg){
  cfg->P = 3; cfg->C = 2; cfg->N = 64;
  cfg->duration_s = 12;
  cfg->burst_size = 40;
  cfg->idle_ms = 250;
  cfg->hwm = 48; cfg->lwm = 32; // H>L (histerese)
  cfg->sample_ms = 100;
}

static void parse_args(int argc, char **argv, config_t *cfg){
  set_defaults(cfg);
  int opt; int a,b;
  while ((opt = getopt(argc, argv, "p:c:n:d:b:i:w:s:h")) != -1){
    switch(opt){
      case 'p': cfg->P = atoi(optarg); break;
      case 'c': cfg->C = atoi(optarg); break;
      case 'n': cfg->N = atoi(optarg); break;
      case 'd': cfg->duration_s = atoi(optarg); break;
      case 'b': cfg->burst_size = atoi(optarg); break;
      case 'i': cfg->idle_ms = atoi(optarg); break;
      case 'w':
        if (sscanf(optarg, "%d:%d", &a, &b)==2){ cfg->hwm=a; cfg->lwm=b; }
        break;
      case 's': cfg->sample_ms = atoi(optarg); break;
      default: break;
    }
  }
  if (cfg->P<1) cfg->P=1;
  if (cfg->C<1) cfg->C=1;
  if (cfg->N<2) cfg->N=2;
  if (cfg->hwm > cfg->N-1) cfg->hwm = cfg->N-1;
  if (cfg->lwm < 0) cfg->lwm = 0;
  if (cfg->hwm <= cfg->lwm) cfg->hwm = cfg->lwm+1;
  if (cfg->sample_ms < 10) cfg->sample_ms = 10;
}

// ---------- buffer circular ----------
typedef struct {
  item_t *buf;
  int cap, head, tail;
  pthread_mutex_t mtx;
  sem_t sem_empty, sem_full;
  // backpressure (histerese)
  pthread_cond_t bp_cv;
  int hwm, lwm;
  int occ; // ocupação atual (protegido por mtx)
} cbuf_t;

static void cbuf_init(cbuf_t *q, int N, int hwm, int lwm){
  q->buf = (item_t*)calloc(N, sizeof(item_t));
  q->cap = N; q->head=0; q->tail=0; q->occ=0;
  pthread_mutex_init(&q->mtx, NULL);
  pthread_cond_init(&q->bp_cv, NULL);
  sem_init(&q->sem_empty, 0, N);
  sem_init(&q->sem_full,  0, 0);
  q->hwm=hwm; q->lwm=lwm;
}
static void cbuf_destroy(cbuf_t *q){
  sem_destroy(&q->sem_empty);
  sem_destroy(&q->sem_full);
  pthread_cond_destroy(&q->bp_cv);
  pthread_mutex_destroy(&q->mtx);
  free(q->buf);
}

static void cbuf_push(cbuf_t *q, item_t x){
  sem_wait(&q->sem_empty);
  pthread_mutex_lock(&q->mtx);
  q->buf[q->tail] = x;
  q->tail = (q->tail + 1) % q->cap;
  q->occ++;
  if (q->occ <= q->lwm) pthread_cond_broadcast(&q->bp_cv);
  pthread_mutex_unlock(&q->mtx);
  sem_post(&q->sem_full);
}
static item_t cbuf_pop(cbuf_t *q){
  sem_wait(&q->sem_full);
  pthread_mutex_lock(&q->mtx);
  item_t x = q->buf[q->head];
  q->head = (q->head + 1) % q->cap;
  q->occ--;
  if (q->occ <= q->lwm) pthread_cond_broadcast(&q->bp_cv);
  pthread_mutex_unlock(&q->mtx);
  sem_post(&q->sem_empty);
  return x;
}

// ---------- métricas globais ----------
typedef struct {
  atomic_long produced;
  atomic_long consumed;
  atomic_long dropped; // reservado
  atomic_llong enq_wait_ns;  // tempo esperando vaga (sem_wait empty)
  atomic_llong deq_wait_ns;  // tempo esperando item (sem_wait full)
  atomic_llong buf_lat_ns;   // latência no buffer (pop - enq)
} metrics_t;

typedef struct {
  config_t cfg;
  cbuf_t q;
  pthread_t *prod, *cons;
  pthread_t sampler;
  atomic_int stop;
  atomic_long next_id;
  metrics_t m;
  // série temporal ocupação
  int sample_ms;
  int max_samples;
  int samples_count;
  struct { int t_ms; int occ; } *samples;
} ctx_t;

static ctx_t G;

// helpers atômicos p/ add em atomic_llong
static inline void add_ll(atomic_llong *dst, long long v){
  atomic_fetch_add_explicit(dst, v, memory_order_relaxed);
}

// wrappers que medem espera (usam semáforos + acumuladores atômicos)
static void cbuf_push_timed(cbuf_t *q, item_t x, atomic_llong *enq_wait){
  long long t0 = now_ns();
  sem_wait(&q->sem_empty);
  add_ll(enq_wait, now_ns() - t0);

  pthread_mutex_lock(&q->mtx);
  q->buf[q->tail] = x;
  q->tail = (q->tail + 1) % q->cap;
  q->occ++;
  if (q->occ <= q->lwm) pthread_cond_broadcast(&q->bp_cv);
  pthread_mutex_unlock(&q->mtx);
  sem_post(&q->sem_full);
}

static item_t cbuf_pop_timed(cbuf_t *q, atomic_llong *deq_wait){
  long long t0 = now_ns();
  sem_wait(&q->sem_full);
  add_ll(deq_wait, now_ns() - t0);

  pthread_mutex_lock(&q->mtx);
  item_t x = q->buf[q->head];
  q->head = (q->head + 1) % q->cap;
  q->occ--;
  if (q->occ <= q->lwm) pthread_cond_broadcast(&q->bp_cv);
  pthread_mutex_unlock(&q->mtx);
  sem_post(&q->sem_empty);
  return x;
}

// ---------- produtores com BURSTS + BACKPRESSURE ----------
static void *producer(void *arg){
  long pid = (long)arg;
  ctx_t *C = &G;
  unsigned seed = (unsigned)(time(NULL) ^ (pid*2654435761u));
  int burst = C->cfg.burst_size;

  while (!atomic_load_explicit(&C->stop, memory_order_relaxed)){
    // BACKPRESSURE: se ocupação >= HWM, aguarda cair abaixo de LWM
    pthread_mutex_lock(&C->q.mtx);
    while (C->q.occ >= C->q.hwm &&
           !atomic_load_explicit(&C->stop, memory_order_relaxed)){
      pthread_cond_wait(&C->q.bp_cv, &C->q.mtx);
    }
    pthread_mutex_unlock(&C->q.mtx);

    // Rajada de B itens "rápidos"
    for (int k=0; k<burst &&
                 !atomic_load_explicit(&C->stop, memory_order_relaxed); k++){
      item_t it;
      it.id = atomic_fetch_add_explicit(&C->next_id, 1, memory_order_relaxed) + 1;
      it.enq_ns = now_ns();
      int jitter = rand_r(&seed) % 3; // 0..2 ms
      if (jitter) sleep_ms(jitter);
      cbuf_push_timed(&C->q, it, &C->m.enq_wait_ns);
      atomic_fetch_add_explicit(&C->m.produced, 1, memory_order_relaxed);
      if ((k & 7)==0) sched_yield();
    }

    // Ociosidade após a rajada
    sleep_ms(C->cfg.idle_ms);
  }
  return NULL;
}

// ---------- consumidores ----------
static void *consumer(void *arg){
  (void)arg;
  ctx_t *C = &G;
  for (;;){
    item_t it = cbuf_pop_timed(&C->q, &C->m.deq_wait_ns);
    if (it.id < 0){ // poison pill
      cbuf_push(&C->q, it); // deixa outra thread ver
      break;
    }
    long long dt = now_ns() - it.enq_ns;
    add_ll(&C->m.buf_lat_ns, dt);
    atomic_fetch_add_explicit(&C->m.consumed, 1, memory_order_relaxed);
    sleep_ms(2); // custo de consumo
  }
  return NULL;
}

// ---------- sampler de ocupação ----------
static void *sampler(void *arg){
  (void)arg;
  ctx_t *C = &G;
  long long t0 = now_ns();
  while (!atomic_load_explicit(&C->stop, memory_order_relaxed)){
    sleep_ms(C->sample_ms);
    int occ;
    pthread_mutex_lock(&C->q.mtx);
    occ = C->q.occ;
    pthread_mutex_unlock(&C->q.mtx);
    int idx = C->samples_count;
    if (idx < C->max_samples){
      C->samples[idx].occ = occ;
      C->samples[idx].t_ms = (int)((now_ns()-t0)/1000000LL);
      C->samples_count++;
    }
  }
  return NULL;
}

// ---------- execução ----------
int main(int argc, char **argv){
  parse_args(argc, argv, &G.cfg);

  // série temporal
  G.sample_ms = G.cfg.sample_ms;
  G.max_samples = (G.cfg.duration_s*1000 / G.sample_ms) + 64;
  G.samples = calloc(G.max_samples, sizeof(*G.samples));
  G.samples_count = 0;

  // init
  cbuf_init(&G.q, G.cfg.N, G.cfg.hwm, G.cfg.lwm);
  G.prod = calloc(G.cfg.P, sizeof(pthread_t));
  G.cons = calloc(G.cfg.C, sizeof(pthread_t));
  atomic_store_explicit(&G.stop, 0, memory_order_relaxed);
  atomic_store_explicit(&G.next_id, 0, memory_order_relaxed);
  memset(&G.m, 0, sizeof(G.m));

  // threads
  for (long i=0;i<G.cfg.P;i++) pthread_create(&G.prod[i], NULL, producer, (void*)i);
  for (long i=0;i<G.cfg.C;i++) pthread_create(&G.cons[i], NULL, consumer, NULL);
  pthread_create(&G.sampler, NULL, sampler, NULL);

  // duração
  long long t0 = now_ns();
  sleep(G.cfg.duration_s);
  atomic_store_explicit(&G.stop, 1, memory_order_relaxed);

  // injeta poison pills (uma por consumidor)
  for (int i=0;i<G.cfg.C;i++){
    item_t poison = {.id = -1, .enq_ns = now_ns()};
    cbuf_push(&G.q, poison);
  }

  // join
  for (int i=0;i<G.cfg.P;i++) pthread_join(G.prod[i], NULL);
  for (int i=0;i<G.cfg.C;i++) pthread_join(G.cons[i], NULL);
  pthread_join(G.sampler, NULL);

  long long t1 = now_ns();
  double elapsed = (t1 - t0)/1e9;

  // métricas
  long prod = atomic_load_explicit(&G.m.produced, memory_order_relaxed);
  long cons = atomic_load_explicit(&G.m.consumed, memory_order_relaxed);
  long long enq_wait = atomic_load_explicit(&G.m.enq_wait_ns, memory_order_relaxed);
  long long deq_wait = atomic_load_explicit(&G.m.deq_wait_ns, memory_order_relaxed);
  long long buf_lat  = atomic_load_explicit(&G.m.buf_lat_ns, memory_order_relaxed);

  double thr_prod = prod/elapsed;
  double thr_cons = cons/elapsed;
  double avg_enq_wait_ms = prod? (enq_wait/1e6)/ (double)prod : 0.0;
  double avg_deq_wait_ms = cons? (deq_wait/1e6)/ (double)cons : 0.0;
  double avg_buf_lat_ms  = cons? (buf_lat /1e6)/ (double)cons : 0.0;

  // resumo
  printf("=== EX2 Estendido (bursts + backpressure + ocupacao) ===\n");
  printf("P=%d C=%d N=%d  dur=%ds  burst=%d idle=%dms  HWM=%d LWM=%d  sample=%dms\n",
         G.cfg.P, G.cfg.C, G.cfg.N, G.cfg.duration_s, G.cfg.burst_size,
         G.cfg.idle_ms, G.cfg.hwm, G.cfg.lwm, G.cfg.sample_ms);
  printf("produced=%ld consumed=%ld elapsed=%.2fs\n", prod, cons, elapsed);
  printf("throughput: prod=%.2f it/s  cons=%.2f it/s\n", thr_prod, thr_cons);
  printf("avg waits:  enq=%.3f ms  deq=%.3f ms  buf-lat=%.3f ms\n",
         avg_enq_wait_ms, avg_deq_wait_ms, avg_buf_lat_ms);

  // série temporal (CSV)
  printf("\n#CSV: t_ms,occ\n");
  for (int i=0;i<G.samples_count;i++){
    printf("%d,%d\n", G.samples[i].t_ms, G.samples[i].occ);
  }

  // limpeza
  cbuf_destroy(&G.q);
  free(G.prod); free(G.cons);
  free(G.samples);

  return 0;
}
