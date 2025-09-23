// pool.c — Thread pool fixo consumindo fila concorrente até EOF.
// Encerramento por poison pills; prova de não-perda/duplicação com vetores "seen".
//
// Entrada (stdin), uma por linha:
//   prime <n>     | primo <n>
//   fib   <n>     | fibo <n> | fibonacci <n>
//
// Uso: ./pool [workers=4] [queue_cap=64] [max_tasks=100000]

#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>

// -------------------- Tarefas --------------------
typedef enum { T_PRIME, T_FIB, T_PILL } TaskKind;

typedef struct {
    TaskKind kind;
    unsigned long long arg;
    unsigned long long id; // 0..(N-1)
} Task;

static inline Task make_task(TaskKind k, unsigned long long arg, unsigned long long id) {
    Task t; t.kind = k; t.arg = arg; t.id = id; return t;
}
static inline Task make_pill(void) { return make_task(T_PILL, 0ULL, ~0ULL); }

// -------------------- Fila concorrente (circular limitada) --------------------
typedef struct {
    Task *buf;
    int cap, head, tail, count;
    pthread_mutex_t mtx;
    pthread_cond_t  not_full, not_empty;
} BQ;

static void bq_init(BQ *q, int cap) {
    q->buf = (Task*)malloc(sizeof(Task)*cap);
    if (!q->buf) { perror("malloc"); exit(1); }
    q->cap = cap; q->head = q->tail = q->count = 0;
    pthread_mutex_init(&q->mtx, NULL);
    pthread_cond_init(&q->not_full, NULL);
    pthread_cond_init(&q->not_empty, NULL);
}
static void bq_destroy(BQ *q) {
    pthread_mutex_destroy(&q->mtx);
    pthread_cond_destroy(&q->not_full);
    pthread_cond_destroy(&q->not_empty);
    free(q->buf);
}
static void bq_put(BQ *q, Task t) {
    pthread_mutex_lock(&q->mtx);
    while (q->count == q->cap) pthread_cond_wait(&q->not_full, &q->mtx);
    q->buf[q->tail] = t;
    q->tail = (q->tail + 1) % q->cap;
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mtx);
}
static Task bq_get(BQ *q) {
    pthread_mutex_lock(&q->mtx);
    while (q->count == 0) pthread_cond_wait(&q->not_empty, &q->mtx);
    Task t = q->buf[q->head];
    q->head = (q->head + 1) % q->cap;
    q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mtx);
    return t;
}

// -------------------- Contexto global --------------------
typedef struct {
    BQ q;
    int workers;

    // Limite superior para tarefas (ids 0..max_tasks-1)
    size_t max_tasks;

    // Contadores
    atomic_ullong enq_cnt;   // tarefas enfileiradas (não inclui pills)
    atomic_ullong deq_cnt;   // retiradas (inclui pills)
    atomic_ullong done_cnt;  // concluídas (não inclui pills)

    // Prova rigorosa: cada id é retirado e concluído exatamente 1 vez
    _Atomic unsigned char *deq_seen;  // tamanho = max_tasks
    _Atomic unsigned char *done_seen; // tamanho = max_tasks

    // Saída serializada
    pthread_mutex_t print_mtx;
} Ctx;

static void slog(Ctx* ctx, const char* fmt, ...) {
    va_list ap;
    pthread_mutex_lock(&ctx->print_mtx);
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fflush(stdout);
    pthread_mutex_unlock(&ctx->print_mtx);
}

// -------------------- CPU-bound --------------------
// Primalidade sem sqrt(): testa até i <= n/i
static bool is_prime_ull(unsigned long long n) {
    if (n < 2ULL) return false;
    if (n % 2ULL == 0ULL) return n == 2ULL;
    if (n % 3ULL == 0ULL) return n == 3ULL;
    for (unsigned long long i = 5ULL; i <= n / i; i += 6ULL) {
        if (n % i == 0ULL || n % (i + 2ULL) == 0ULL) return false;
    }
    return true;
}

static unsigned long long fib_iter(unsigned long long n) {
    if (n == 0ULL) return 0ULL;
    unsigned long long a = 0ULL, b = 1ULL;
    for (unsigned long long i = 1ULL; i < n; ++i) {
        unsigned long long s = a + b;
        a = b; b = s;
    }
    return b;
}

// -------------------- Worker --------------------
typedef struct { int idx; Ctx* ctx; } WorkerArg;

static void* worker_fn(void* arg) {
    WorkerArg* wa = (WorkerArg*)arg;
    Ctx* ctx = wa->ctx;

    for (;;) {
        Task t = bq_get(&ctx->q);
        atomic_fetch_add_explicit(&ctx->deq_cnt, 1ULL, memory_order_relaxed);

        if (t.kind == T_PILL) break;

        // Marca retirada única
        if (t.id >= ctx->max_tasks) {
            fprintf(stderr, "ERRO: id %llu >= max_tasks\n", t.id);
            exit(2);
        }
        unsigned char old = atomic_fetch_add_explicit(&ctx->deq_seen[t.id], 1, memory_order_relaxed);
        if (old != 0) {
            fprintf(stderr, "ERRO: tarefa id=%llu retirada mais de uma vez\n", t.id);
            exit(2);
        }

        switch (t.kind) {
            case T_PRIME: {
                bool p = is_prime_ull(t.arg);
                slog(ctx, "[prime] id=%llu n=%llu => %s\n",
                     t.id, t.arg, p ? "prime" : "composite");
            } break;
            case T_FIB: {
                unsigned long long f = fib_iter(t.arg);
                slog(ctx, "[fib]   id=%llu n=%llu => %llu\n", t.id, t.arg, f);
            } break;
            default: break;
        }

        // Marca conclusão única
        old = atomic_fetch_add_explicit(&ctx->done_seen[t.id], 1, memory_order_relaxed);
        if (old != 0) {
            fprintf(stderr, "ERRO: tarefa id=%llu concluída mais de uma vez\n", t.id);
            exit(2);
        }
        atomic_fetch_add_explicit(&ctx->done_cnt, 1ULL, memory_order_relaxed);
    }
    return NULL;
}

// -------------------- Helpers de string --------------------
static void rstrip_crlf(char *s) {
    if (!s) return;
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r')) { s[--n] = '\0'; }
}
static const char* lskip_spaces(const char* s) {
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}
static int is_comment_or_empty(const char* s) {
    s = lskip_spaces(s);
    return (*s == '\0' || *s == '#');
}

// -------------------- Parsing robusto --------------------
static bool parse_line(const char* raw, Task* out, unsigned long long id) {
    char line[256];
    // Copia com limite e normaliza CR/LF
    snprintf(line, sizeof(line), "%s", raw);
    rstrip_crlf(line);

    if (is_comment_or_empty(line)) return false;

    // normaliza para minúsculas a palavra comando
    char cmd[32] = {0};
    unsigned long long n = 0ULL;

    // permite: prime/primo | fib/fibo/fibonacci
    if (sscanf(line, " %31s %llu", cmd, &n) >= 2) {
        for (char* p = cmd; *p; ++p) *p = (char)tolower((unsigned char)*p);

        if (strcmp(cmd, "prime") == 0 || strcmp(cmd, "primo") == 0) {
            *out = make_task(T_PRIME, n, id);
            return true;
        }
        if (strcmp(cmd, "fib") == 0 || strcmp(cmd, "fibo") == 0 || strcmp(cmd, "fibonacci") == 0) {
            *out = make_task(T_FIB, n, id);
            return true;
        }
        fprintf(stderr, "Comando desconhecido: '%s' (use: prime <n> ou fib <n>)\n", cmd);
        return false;
    } else {
        // formato sem número ou vazio
        fprintf(stderr, "Formato inválido (esperado: 'prime <n>' ou 'fib <n>'): %s\n", line);
        return false;
    }
}

// -------------------- Main --------------------
int main(int argc, char** argv) {
    int workers = 4;
    int qcap = 64;
    size_t max_tasks = 100000;

    if (argc >= 2) workers = atoi(argv[1]);
    if (argc >= 3) qcap    = atoi(argv[2]);
    if (argc >= 4) {
        long long tmp = atoll(argv[3]);
        if (tmp <= 0) { fprintf(stderr, "max_tasks inválido\n"); return 1; }
        max_tasks = (size_t)tmp;
    }
    if (workers <= 0 || qcap <= 0) {
        fprintf(stderr, "Uso: %s [workers=4] [queue_cap=64] [max_tasks=100000]\n", argv[0]);
        return 1;
    }

    Ctx ctx;
    bq_init(&ctx.q, qcap);
    ctx.workers   = workers;
    ctx.max_tasks = max_tasks;

    atomic_init(&ctx.enq_cnt,  0ULL);
    atomic_init(&ctx.deq_cnt,  0ULL);
    atomic_init(&ctx.done_cnt, 0ULL);

    ctx.deq_seen  = (_Atomic unsigned char*)calloc(max_tasks, sizeof(_Atomic unsigned char));
    ctx.done_seen = (_Atomic unsigned char*)calloc(max_tasks, sizeof(_Atomic unsigned char));
    if (!ctx.deq_seen || !ctx.done_seen) { perror("calloc"); return 1; }

    pthread_mutex_init(&ctx.print_mtx, NULL);

    // Cria pool
    pthread_t* tids = (pthread_t*)malloc(sizeof(pthread_t)*workers);
    WorkerArg* wargs = (WorkerArg*)malloc(sizeof(WorkerArg)*workers);
    if (!tids || !wargs) { perror("malloc"); return 1; }

    for (int i = 0; i < workers; ++i) {
        wargs[i].idx = i; wargs[i].ctx = &ctx;
        if (pthread_create(&tids[i], NULL, worker_fn, &wargs[i]) != 0) {
            perror("pthread_create"); return 1;
        }
    }

    // Produtor: lê stdin até EOF
    char *line = NULL;
    size_t len = 0;
    ssize_t r;
    unsigned long long next_id = 0ULL;

    while ((r = getline(&line, &len, stdin)) != -1) {
        rstrip_crlf(line);
        if (is_comment_or_empty(line)) continue;

        Task t;
        if (parse_line(line, &t, next_id)) {
            if (next_id >= ctx.max_tasks) {
                fprintf(stderr, "ERRO: excedeu max_tasks=%zu (passe um valor maior)\n", ctx.max_tasks);
                free(line);
                return 1;
            }
            bq_put(&ctx.q, t);
            atomic_fetch_add_explicit(&ctx.enq_cnt, 1ULL, memory_order_relaxed);
            next_id++;
        } else {
            // já diagnosticado em parse_line; segue leitura
            continue;
        }
    }
    free(line);

    // Envia W poison pills para encerrar os workers
    for (int i = 0; i < workers; ++i) bq_put(&ctx.q, make_pill());

    // Aguarda workers
    for (int i = 0; i < workers; ++i) pthread_join(tids[i], NULL);

    // Coleta métricas
    unsigned long long enq  = atomic_load_explicit(&ctx.enq_cnt,  memory_order_relaxed);
    unsigned long long deq  = atomic_load_explicit(&ctx.deq_cnt,  memory_order_relaxed);
    unsigned long long done = atomic_load_explicit(&ctx.done_cnt, memory_order_relaxed);

    // Sanidade
    if (deq != enq + (unsigned long long)workers) {
        fprintf(stderr, "ERRO: deq(%llu) != enq(%llu) + workers(%d)\n", deq, enq, workers);
        return 2;
    }
    if (done != enq) {
        fprintf(stderr, "ERRO: done(%llu) != enq(%llu)\n", done, enq);
        return 2;
    }

    // Prova rigorosa: cada id em [0..enq-1] retirado e concluído exatamente 1 vez
    for (unsigned long long i = 0ULL; i < enq; ++i) {
        unsigned char d = atomic_load_explicit(&ctx.deq_seen[i],  memory_order_relaxed);
        unsigned char c = atomic_load_explicit(&ctx.done_seen[i], memory_order_relaxed);
        if (d != 1) { fprintf(stderr, "ERRO: id=%llu retirado %u vez(es)\n", i, d); return 2; }
        if (c != 1) { fprintf(stderr, "ERRO: id=%llu concluído %u vez(es)\n", i, c); return 2; }
    }

    printf("\nResumo:\n");
    printf("  Enfileiradas:  %llu\n", enq);
    printf("  Retiradas:     %llu (inclui %d pills)\n", deq, workers);
    printf("  Concluídas:    %llu\n", done);
    printf("  OK: fila thread-safe, nenhuma tarefa perdida/duplicada, encerramento limpo.\n");

    // Cleanup
    bq_destroy(&ctx.q);
    pthread_mutex_destroy(&ctx.print_mtx);
    free((void*)ctx.deq_seen);
    free((void*)ctx.done_seen);
    free(tids);
    free(wargs);
    return 0;
}
