// pool.c — Thread pool fixo com fila concorrente.
// Compatível com OnlineGDB (Linux, gcc).
// Rodando com parâmetros DENTRO do programa (sem stdin).
//
// Se quiser ler da entrada padrão, mude USE_STDIN para 1.
//
// Compilar localmente (opcional): gcc -O2 -pthread -std=c11 -o pool pool.c

#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ================== PARÂMETROS NO CÓDIGO ==================
enum { NTHREADS = 4 };     // número de threads do pool
enum { USE_STDIN = 0 };    // 0 = usa LINES[] abaixo; 1 = lê stdin
enum { MAX_LINE = 256 };   // tamanho máx de uma linha

// Tarefas pré-definidas (quando USE_STDIN = 0)
static const char *LINES[] = {
    "prime 97",
    "fib 45",
    "primo 1000003",
    "fibonacci 40",
    "prime 10000019",
    "fib 46",
    "prime 99991",
    "fib 30",
    NULL // marcador de fim
};
// ===========================================================

// ---- Modelo de tarefa ----
typedef enum { TASK_PRIME, TASK_FIB } task_kind;

typedef struct task {
    int id;                 // id único (diagnóstico)
    task_kind kind;
    unsigned long long n;   // argumento
    struct task *next;
} task_t;

// ---- Fila concorrente: lista encadeada + mutex/condvar ----
typedef struct {
    task_t *head, *tail;
    int size;
    int done;               // 0 = ainda chegando tarefas; 1 = produtor encerrou
    pthread_mutex_t mtx;
    pthread_cond_t  not_empty;
} queue_t;

static void q_init(queue_t *q) {
    q->head = q->tail = NULL;
    q->size = 0;
    q->done = 0;
    pthread_mutex_init(&q->mtx, NULL);
    pthread_cond_init(&q->not_empty, NULL);
}

static void q_destroy(queue_t *q) {
    pthread_mutex_destroy(&q->mtx);
    pthread_cond_destroy(&q->not_empty);
}

static void q_push(queue_t *q, task_t *t) {
    t->next = NULL;
    pthread_mutex_lock(&q->mtx);
    if (q->tail) q->tail->next = t; else q->head = t;
    q->tail = t;
    q->size++;
    pthread_cond_signal(&q->not_empty);  // acorda 1 worker
    pthread_mutex_unlock(&q->mtx);
}

static task_t* q_pop(queue_t *q) {
    pthread_mutex_lock(&q->mtx);
    while (q->size == 0 && !q->done) {
        pthread_cond_wait(&q->not_empty, &q->mtx);
    }
    if (q->size == 0 && q->done) {
        pthread_mutex_unlock(&q->mtx);
        return NULL; // encerramento
    }
    task_t *t = q->head;
    q->head = t->next;
    if (!q->head) q->tail = NULL;
    q->size--;
    pthread_mutex_unlock(&q->mtx);
    return t;
}

static void q_close(queue_t *q) {
    pthread_mutex_lock(&q->mtx);
    q->done = 1;
    pthread_cond_broadcast(&q->not_empty);
    pthread_mutex_unlock(&q->mtx);
}

// ---- Cargas CPU-bound ----
static int is_prime_ull(unsigned long long x) {
    if (x < 2ULL) return 0;
    if ((x % 2ULL) == 0ULL) return x == 2ULL;
    for (unsigned long long d = 3ULL; d*d <= x; d += 2ULL) {
        if ((x % d) == 0ULL) return 0;
    }
    return 1;
}

static unsigned long long fib_iter(unsigned long long n) {
    if (n <= 1ULL) return n;
    unsigned long long a = 0ULL, b = 1ULL;
    for (unsigned long long i = 2ULL; i <= n; ++i) {
        unsigned long long c = a + b;
        a = b;
        b = c;
    }
    return b;
}

// ---- Globais de coordenação / métricas ----
static queue_t gq;
static pthread_mutex_t print_mtx = PTHREAD_MUTEX_INITIALIZER;

// contadores protegidos por mutex (compatível com qualquer gcc)
static int enq_count = 0;
static int proc_count = 0;
static pthread_mutex_t cnt_mtx = PTHREAD_MUTEX_INITIALIZER;

static void inc_enq(void){
    pthread_mutex_lock(&cnt_mtx);
    enq_count++;
    pthread_mutex_unlock(&cnt_mtx);
}
static void inc_proc(void){
    pthread_mutex_lock(&cnt_mtx);
    proc_count++;
    pthread_mutex_unlock(&cnt_mtx);
}

// ---- Worker ----
static void* worker_main(void *arg) {
    (void)arg;
    for (;;) {
        task_t *t = q_pop(&gq);
        if (!t) break; // fim
        if (t->kind == TASK_PRIME) {
            int p = is_prime_ull(t->n);
            pthread_mutex_lock(&print_mtx);
            printf("[thr %lu] id=%d prime(%llu) -> %s\n",
                   (unsigned long)pthread_self(), t->id,
                   (unsigned long long)t->n, p ? "true" : "false");
            pthread_mutex_unlock(&print_mtx);
        } else {
            unsigned long long f = fib_iter(t->n);
            pthread_mutex_lock(&print_mtx);
            printf("[thr %lu] id=%d fib(%llu) -> %llu\n",
                   (unsigned long)pthread_self(), t->id,
                   (unsigned long long)t->n, (unsigned long long)f);
            pthread_mutex_unlock(&print_mtx);
        }
        inc_proc();
        free(t);
    }
    return NULL;
}

// ---- Utilitário: case-insensitive "starts_with" simples ----
static int starts_with(const char *s, const char *kw) {
    while (*kw && *s) {
        char a = *s, b = *kw;
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
        s++; kw++;
    }
    return *kw == '\0' && (*s == '\0' || *s == ' ' || *s == '\t' || *s == '\n');
}

int main(void) {
    q_init(&gq);

    // Cria pool
    pthread_t th[NTHREADS];
    for (int i = 0; i < NTHREADS; ++i) {
        if (pthread_create(&th[i], NULL, worker_main, NULL) != 0) {
            perror("pthread_create");
            return 1;
        }
    }

    // Produtor: OU lê stdin, OU usa LINES[] interno
    int next_id = 0;
    if (USE_STDIN) {
        char line[MAX_LINE];
        while (fgets(line, sizeof(line), stdin)) {
            line[strcspn(line, "\r\n")] = '\0';
            if (line[0] == '\0' || line[0] == '#') continue;

            char cmd[32] = {0};
            unsigned long long n = 0ULL;
            if (sscanf(line, "%31s %llu", cmd, &n) != 2) {
                fprintf(stderr, "Linha inválida: \"%s\"\n", line);
                continue;
            }

            task_t *t = (task_t*)malloc(sizeof(task_t));
            if (!t) { perror("malloc"); break; }

            if (starts_with(cmd, "prime") || starts_with(cmd, "primo")) {
                t->kind = TASK_PRIME;
            } else if (starts_with(cmd, "fib") || starts_with(cmd, "fibonacci")) {
                t->kind = TASK_FIB;
            } else {
                fprintf(stderr, "Comando desconhecido: \"%s\"\n", cmd);
                free(t);
                continue;
            }
            t->id = next_id++;
            t->n  = n;
            q_push(&gq, t);
            inc_enq();
        }
    } else {
        // Usa tarefas embutidas
        for (int i = 0; LINES[i] != NULL; ++i) {
            const char *line = LINES[i];
            char cmd[32] = {0};
            unsigned long long n = 0ULL;
            if (sscanf(line, "%31s %llu", cmd, &n) != 2) {
                fprintf(stderr, "Linha inválida (LINES[%d]): \"%s\"\n", i, line);
                continue;
            }
            task_t *t = (task_t*)malloc(sizeof(task_t));
            if (!t) { perror("malloc"); break; }

            if (starts_with(cmd, "prime") || starts_with(cmd, "primo")) {
                t->kind = TASK_PRIME;
            } else if (starts_with(cmd, "fib") || starts_with(cmd, "fibonacci")) {
                t->kind = TASK_FIB;
            } else {
                fprintf(stderr, "Comando desconhecido (LINES[%d]): \"%s\"\n", i, cmd);
                free(t);
                continue;
            }
            t->id = next_id++;
            t->n  = n;
            q_push(&gq, t);
            inc_enq();
        }
    }

    // Sinaliza fim de produção
    q_close(&gq);

    // Aguarda workers
    for (int i = 0; i < NTHREADS; ++i) pthread_join(th[i], NULL);
    q_destroy(&gq);

    // Verificação de não-perda
    pthread_mutex_lock(&cnt_mtx);
    int enq = enq_count, pro = proc_count;
    pthread_mutex_unlock(&cnt_mtx);

    fprintf(stderr, "\nResumo: enfileiradas=%d processadas=%d %s\n",
            enq, pro, (enq == pro ? "[OK]" : "[ERRO: divergência]"));

    return (enq == pro) ? 0 : 2;
}
