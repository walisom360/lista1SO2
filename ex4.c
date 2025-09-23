// pipeline.c
// Pipeline 3 estágios: captura -> processamento -> gravação
// Duas filas limitadas com mutex + cond (sem busy-wait).
// Encerramento limpo por poison pill (id = -1).
// Demonstra ausência de deadlock e perda de itens via asserts.

#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <time.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

// -------------------- Item & Poison Pill --------------------
typedef struct {
    int id;         // 0..N-1; -1 = poison pill
    int payload;    // dado de exemplo (pode ser qualquer coisa)
} Item;

static inline Item make_item(int id, int payload) { Item it = {id, payload}; return it; }
static const Item POISON = { .id = -1, .payload = 0 };

// -------------------- Fila Limitada (circular) --------------------
typedef struct {
    Item *buf;
    int capacity;
    int head;     // próximo a sair
    int tail;     // próximo a entrar
    int count;

    pthread_mutex_t mtx;
    pthread_cond_t  not_full;
    pthread_cond_t  not_empty;
} BQueue;

static void bq_init(BQueue *q, int capacity) {
    q->buf = (Item*)malloc(sizeof(Item) * capacity);
    if (!q->buf) { perror("malloc"); exit(1); }
    q->capacity = capacity;
    q->head = q->tail = q->count = 0;
    pthread_mutex_init(&q->mtx, NULL);
    pthread_cond_init(&q->not_full, NULL);
    pthread_cond_init(&q->not_empty, NULL);
}

static void bq_destroy(BQueue *q) {
    pthread_mutex_destroy(&q->mtx);
    pthread_cond_destroy(&q->not_full);
    pthread_cond_destroy(&q->not_empty);
    free(q->buf);
}

// Enfileira (bloqueante se cheio)
static void bq_put(BQueue *q, Item it) {
    pthread_mutex_lock(&q->mtx);
    while (q->count == q->capacity) {
        pthread_cond_wait(&q->not_full, &q->mtx);
    }
    q->buf[q->tail] = it;
    q->tail = (q->tail + 1) % q->capacity;
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mtx);
}

// Desenfileira (bloqueante se vazio)
static Item bq_get(BQueue *q) {
    pthread_mutex_lock(&q->mtx);
    while (q->count == 0) {
        pthread_cond_wait(&q->not_empty, &q->mtx);
    }
    Item it = q->buf[q->head];
    q->head = (q->head + 1) % q->capacity;
    q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mtx);
    return it;
}

// -------------------- Contexto global --------------------
typedef struct {
    int N;
    BQueue q_cap_to_proc;
    BQueue q_proc_to_save;

    // Métricas e verificação
    atomic_int produced;
    atomic_int processed;
    atomic_int saved;

    bool *seen_proc; // tamanho N: marca ids processados
    bool *seen_save; // tamanho N: marca ids salvos
} Context;

static void msleep(int ms) {
    struct timespec ts = {.tv_sec = ms/1000, .tv_nsec = (long)(ms%1000)*1000000L};
    nanosleep(&ts, NULL);
}

// -------------------- Thread: Captura (produtor) --------------------
static void* capture_thread(void *arg) {
    Context *ctx = (Context*)arg;
    for (int i = 0; i < ctx->N; i++) {
        // Simula “capturar” algo (pequena variação temporal)
        // (dorme bem pouco para exercitar a fila limitada)
        // msleep(rand()%2); // opcional
        Item it = make_item(i, i*10);
        bq_put(&ctx->q_cap_to_proc, it);
        atomic_fetch_add_explicit(&ctx->produced, 1, memory_order_relaxed);
    }
    // Envia poison pill para encerrar o processamento
    bq_put(&ctx->q_cap_to_proc, POISON);
    return NULL;
}

// -------------------- Thread: Processamento (intermediário) --------------------
static void* process_thread(void *arg) {
    Context *ctx = (Context*)arg;
    while (1) {
        Item it = bq_get(&ctx->q_cap_to_proc);
        if (it.id == -1) {
            // Encerramento: propaga poison pill e finaliza.
            bq_put(&ctx->q_proc_to_save, POISON);
            break;
        }
        // “Processa” o item (ex.: dobra o payload)
        it.payload *= 2;

        // Marca id visto no processamento (sem corrida: thread única)
        if (it.id >= 0 && it.id < ctx->N) {
            if (ctx->seen_proc[it.id]) {
                fprintf(stderr, "ERRO: item %d processado em duplicidade!\n", it.id);
                exit(2);
            }
            ctx->seen_proc[it.id] = true;
        } else {
            fprintf(stderr, "ERRO: id fora de faixa no processamento: %d\n", it.id);
            exit(2);
        }

        atomic_fetch_add_explicit(&ctx->processed, 1, memory_order_relaxed);
        bq_put(&ctx->q_proc_to_save, it);
    }
    return NULL;
}

// -------------------- Thread: Gravação (consumidor) --------------------
static void* save_thread(void *arg) {
    Context *ctx = (Context*)arg;
    while (1) {
        Item it = bq_get(&ctx->q_proc_to_save);
        if (it.id == -1) {
            // Encerramento: fim do consumidor.
            break;
        }

        // “Grava” (simulação)
        // msleep(rand()%2); // opcional
        // Marca id visto na gravação
        if (ctx->seen_save[it.id]) {
            fprintf(stderr, "ERRO: item %d salvo em duplicidade!\n", it.id);
            exit(2);
        }
        ctx->seen_save[it.id] = true;

        atomic_fetch_add_explicit(&ctx->saved, 1, memory_order_relaxed);
    }
    return NULL;
}

// -------------------- Main --------------------
int main(int argc, char **argv) {
    int N = 100;
    int cap1 = 4;
    int cap2 = 4;
    if (argc >= 2) N = atoi(argv[1]);
    if (argc >= 3) cap1 = atoi(argv[2]);
    if (argc >= 4) cap2 = atoi(argv[3]);
    if (N <= 0 || cap1 <= 0 || cap2 <= 0) {
        fprintf(stderr, "Uso: %s [N=100] [cap1=4] [cap2=4]\n", argv[0]);
        return 1;
    }

    srand((unsigned)time(NULL));

    Context ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.N = N;
    bq_init(&ctx.q_cap_to_proc, cap1);
    bq_init(&ctx.q_proc_to_save, cap2);
    atomic_init(&ctx.produced, 0);
    atomic_init(&ctx.processed, 0);
    atomic_init(&ctx.saved, 0);

    ctx.seen_proc = (bool*)calloc((size_t)N, sizeof(bool));
    ctx.seen_save = (bool*)calloc((size_t)N, sizeof(bool));
    if (!ctx.seen_proc || !ctx.seen_save) { perror("calloc"); return 1; }

    pthread_t th_cap, th_proc, th_save;

    if (pthread_create(&th_cap,  NULL, capture_thread,  &ctx) != 0) { perror("pthread_create cap"); return 1; }
    if (pthread_create(&th_proc, NULL, process_thread,  &ctx) != 0) { perror("pthread_create proc"); return 1; }
    if (pthread_create(&th_save, NULL, save_thread,     &ctx) != 0) { perror("pthread_create save"); return 1; }

    // Aguarda término limpo (sem deadlock)
    pthread_join(th_cap,  NULL);
    pthread_join(th_proc, NULL);
    pthread_join(th_save, NULL);

    // Verificações finais (sem perda/duplicação)
    int prod = atomic_load_explicit(&ctx.produced, memory_order_relaxed);
    int proc = atomic_load_explicit(&ctx.processed, memory_order_relaxed);
    int save = atomic_load_explicit(&ctx.saved,     memory_order_relaxed);

    printf("Resumo:\n");
    printf("  Produzidos: %d\n", prod);
    printf("  Processados: %d\n", proc);
    printf("  Gravados:   %d\n", save);

    // Todos os N devem ter sido processados e gravados
    assert(prod == N);
    assert(proc == N);
    assert(save == N);

    // Checa marcas de todos os IDs 0..N-1
    for (int i = 0; i < N; i++) {
        if (!ctx.seen_proc[i]) {
            fprintf(stderr, "ERRO: item %d não foi processado!\n", i);
            return 2;
        }
        if (!ctx.seen_save[i]) {
            fprintf(stderr, "ERRO: item %d não foi gravado!\n", i);
            return 2;
        }
    }

    puts("OK: pipeline encerrou sem deadlock e sem perda de itens (com poison pill).");

    free(ctx.seen_proc);
    free(ctx.seen_save);
    bq_destroy(&ctx.q_cap_to_proc);
    bq_destroy(&ctx.q_proc_to_save);
    return 0;
}
