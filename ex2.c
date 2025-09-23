// pc_buffer.c
// Produtores/Consumidores com buffer circular, semáforos e métricas.
// Autor: você :)  — foco didático

#define _GNU_SOURCE
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

// ------------------------------
// Util: tempo monotônico (ns/s)
// ------------------------------
static inline long long nsec_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + (long long)ts.tv_nsec;
}

static inline double sec_from_ns(long long ns) {
    return (double)ns / 1e9;
}

// ------------------------------
// Item colocado no buffer
// ------------------------------
typedef struct {
    long id;                // id >= 0 => item real; id == -1 => "pílula venenosa" (encerra consumidor)
    long long enq_t_ns;     // quando entrou no buffer (para medir latência no buffer)
} item_t;

// ------------------------------
// Buffer circular
// ------------------------------
typedef struct {
    item_t *v;
    int cap;            // capacidade N
    int head;           // remove de head
    int tail;           // insere em tail
} circbuf_t;

static circbuf_t buf;
static pthread_mutex_t buf_mtx = PTHREAD_MUTEX_INITIALIZER;
static sem_t sem_empty; // vagas
static sem_t sem_full;  // itens

// ------------------------------
// Parâmetros globais
// ------------------------------
static int    N = 8;           // tamanho buffer
static int    P = 2;           // produtores
static int    C = 2;           // consumidores
static long   M = 50000;       // total de itens reais
static int    PROD_MAX_US = 2000; // delay max produtor por item
static int    CONS_MAX_US = 2000; // delay max consumidor por item

// ------------------------------
// Métricas globais
// (usar atomics + um mutex para acumulações de ponto flutuante)
// ------------------------------
static atomic_long produced_count = 0;  // incrementado por produtores (itens reais)
static atomic_long consumed_count = 0;  // incrementado por consumidores (itens reais)

static pthread_mutex_t metrics_mtx = PTHREAD_MUTEX_INITIALIZER;
static long long total_prod_wait_ns = 0;   // tempo total produtores bloqueados (espera por vaga+mutex)
static long long total_cons_wait_ns = 0;   // tempo total consumidores bloqueados (espera por item+mutex)
static long long total_buffer_lat_ns = 0;  // tempo total dentro do buffer (enq->deq)

// ------------------------------
// Acesso ao buffer (em região crítica)
// ------------------------------
static void buf_init(int cap) {
    buf.v = (item_t*)calloc(cap, sizeof(item_t));
    buf.cap = cap;
    buf.head = 0;
    buf.tail = 0;
}

static void buf_free(void) {
    free(buf.v);
}

static void buf_push(item_t it) {
    // supõe semáforo já garantiu vaga
    buf.v[buf.tail] = it;
    buf.tail = (buf.tail + 1) % buf.cap;
}

static item_t buf_pop(void) {
    // supõe semáforo já garantiu item
    item_t it = buf.v[buf.head];
    buf.head = (buf.head + 1) % buf.cap;
    return it;
}

// ------------------------------
// RNG simples p/ delays
// ------------------------------
static inline int rand_us(int max_us) {
    if (max_us <= 0) return 0;
    return rand() % (max_us + 1);
}

// ------------------------------
// Produtor
// Cada produtor gera um número (quase) igual de itens reais.
// Medimos tempo de espera (do "pronto para enfileirar" até após push).
// ------------------------------
typedef struct {
    int id;
    long my_quota;
} producer_arg_t;

void* producer_thread(void* arg) {
    producer_arg_t* pa = (producer_arg_t*)arg;

    for (long k = 0; k < pa->my_quota; k++) {
        // Simula "produção" do item
        usleep(rand_us(PROD_MAX_US));

        long long t0 = nsec_now();

        // Espera vaga (sem busy-wait)
        sem_wait(&sem_empty);

        // Entra na região crítica do buffer
        pthread_mutex_lock(&buf_mtx);

        long long t1 = nsec_now(); // após conseguir vaga + mutex (isso inclui espera real do produtor)

        // Prepara item real
        item_t it;
        it.id = atomic_fetch_add(&produced_count, 1); // id único crescente (0..M-1)
        it.enq_t_ns = t1; // instante de enfileiramento

        // Enfileira
        buf_push(it);

        pthread_mutex_unlock(&buf_mtx);

        // Sinaliza que há item disponível
        sem_post(&sem_full);

        // Acumula tempo de espera do produtor
        pthread_mutex_lock(&metrics_mtx);
        total_prod_wait_ns += (t1 - t0);
        pthread_mutex_unlock(&metrics_mtx);
    }

    return NULL;
}

// ------------------------------
// Consumidor
// Fica retirando itens até receber a "pílula venenosa" (id == -1).
// Medimos:
//   - tempo de espera do consumidor (sem_wait até entrar no buffer)
//   - latência no buffer (de enq a deq)
// ------------------------------
typedef struct {
    int id;
} consumer_arg_t;

void* consumer_thread(void* arg) {
    (void)arg;

    for (;;) {
        long long t0 = nsec_now();

        // Espera item (sem busy-wait)
        sem_wait(&sem_full);

        // Entra na região crítica do buffer
        pthread_mutex_lock(&buf_mtx);

        long long t1 = nsec_now(); // após conseguir item + mutex (espera real do consumidor)

        item_t it = buf_pop();

        pthread_mutex_unlock(&buf_mtx);

        // Libera vaga
        sem_post(&sem_empty);

        // Se for pílula venenosa, encerra
        if (it.id == -1) {
            // Não contabiliza métrica de latência/consumo para sentinela
            pthread_mutex_lock(&metrics_mtx);
            total_cons_wait_ns += (t1 - t0);
            pthread_mutex_unlock(&metrics_mtx);
            break;
        }

        // Latência dentro do buffer
        long long deq_t_ns = t1;
        long long inbuf_ns = (deq_t_ns - it.enq_t_ns);

        // "Processa" o item
        usleep(rand_us(CONS_MAX_US));

        // Conta item consumido real
        atomic_fetch_add(&consumed_count, 1);

        // Acumula métricas
        pthread_mutex_lock(&metrics_mtx);
        total_cons_wait_ns += (t1 - t0);
        total_buffer_lat_ns += inbuf_ns;
        pthread_mutex_unlock(&metrics_mtx);
    }

    return NULL;
}

// ------------------------------
// Inserir "pílulas venenosas" para acordar/encerrar consumidores
// (uma por consumidor)
// ------------------------------
static void inject_poison_pills(int count) {
    for (int i = 0; i < count; i++) {
        sem_wait(&sem_empty);
        pthread_mutex_lock(&buf_mtx);
        item_t it;
        it.id = -1;
        it.enq_t_ns = nsec_now();
        buf_push(it);
        pthread_mutex_unlock(&buf_mtx);
        sem_post(&sem_full);
    }
}

// ------------------------------
// Main
// ------------------------------
static void usage(const char* prog) {
    fprintf(stderr,
        "Uso: %s [N] [P] [C] [M] [prod_max_us] [cons_max_us]\n"
        "Ex.: %s 8 2 2 50000 2000 2000\n", prog, prog);
}

int main(int argc, char** argv) {
    // Seed do RNG
    srand((unsigned)time(NULL) ^ (unsigned)getpid());

    // Parâmetros (com defaults)
    if (argc >= 2) N = atoi(argv[1]);
    if (argc >= 3) P = atoi(argv[2]);
    if (argc >= 4) C = atoi(argv[3]);
    if (argc >= 5) M = atol(argv[4]);
    if (argc >= 6) PROD_MAX_US = atoi(argv[5]);
    if (argc >= 7) CONS_MAX_US = atoi(argv[6]);

    if (N <= 0 || P <= 0 || C <= 0 || M <= 0) {
        usage(argv[0]);
        return 1;
    }

    printf("Config: N=%d, P=%d, C=%d, M=%ld, prod_max_us=%d, cons_max_us=%d\n",
           N, P, C, M, PROD_MAX_US, CONS_MAX_US);

    buf_init(N);
    sem_init(&sem_empty, 0, N);
    sem_init(&sem_full,  0, 0);

    pthread_t *prod_threads = (pthread_t*)malloc(sizeof(pthread_t)*P);
    pthread_t *cons_threads = (pthread_t*)malloc(sizeof(pthread_t)*C);
    producer_arg_t *pargs   = (producer_arg_t*)malloc(sizeof(producer_arg_t)*P);
    consumer_arg_t *cargs   = (consumer_arg_t*)malloc(sizeof(consumer_arg_t)*C);

    // Divide M entre produtores (distribuição quase uniforme)
    long base = M / P;
    long rem  = M % P;

    long long t_start = nsec_now();

    for (int i = 0; i < P; i++) {
        pargs[i].id = i;
        pargs[i].my_quota = base + (i < rem ? 1 : 0);
        if (pthread_create(&prod_threads[i], NULL, producer_thread, &pargs[i]) != 0) {
            fprintf(stderr, "Erro ao criar produtor %d: %s\n", i, strerror(errno));
            return 2;
        }
    }

    for (int i = 0; i < C; i++) {
        cargs[i].id = i;
        if (pthread_create(&cons_threads[i], NULL, consumer_thread, &cargs[i]) != 0) {
            fprintf(stderr, "Erro ao criar consumidor %d: %s\n", i, strerror(errno));
            return 3;
        }
    }

    // Espera produtores terminarem (garante que M itens reais foram colocados)
    for (int i = 0; i < P; i++) {
        pthread_join(prod_threads[i], NULL);
    }

    // Injeta C pílulas para encerrar consumidores que ainda estiverem bloqueados
    inject_poison_pills(C);

    // Espera consumidores
    for (int i = 0; i < C; i++) {
        pthread_join(cons_threads[i], NULL);
    }

    long long t_end = nsec_now();

    // Métricas finais
    long   prod_ok = atomic_load(&produced_count);
    long   cons_ok = atomic_load(&consumed_count);
    double elapsed_s = sec_from_ns(t_end - t_start);

    // Evitar divisão por zero
    double m_count = (cons_ok > 0 ? (double)cons_ok : 1.0);

    double throughput_ips   = cons_ok / elapsed_s; // items per second
    double avg_prod_wait_ms = (total_prod_wait_ns / (double) (prod_ok ? prod_ok : 1)) / 1e6;
    double avg_cons_wait_ms = (total_cons_wait_ns / (double) (cons_ok ? cons_ok : 1)) / 1e6;
    double avg_inbuf_ms     = (total_buffer_lat_ns / m_count) / 1e6;

    printf("\n==== Resultados ====\n");
    printf("Tempo total: %.3f s\n", elapsed_s);
    printf("Produzidos (reais): %ld | Consumidos (reais): %ld\n", prod_ok, cons_ok);
    printf("Throughput: %.2f itens/s\n", throughput_ips);
    printf("Tempo médio de espera do PRODUTOR: %.3f ms/item\n", avg_prod_wait_ms);
    printf("Tempo médio de espera do CONSUMIDOR: %.3f ms/item\n", avg_cons_wait_ms);
    printf("Latência média no buffer (enfileirar->desenfileirar): %.3f ms/item\n", avg_inbuf_ms);

    // Limpeza
    sem_destroy(&sem_empty);
    sem_destroy(&sem_full);
    buf_free();
    free(prod_threads);
    free(cons_threads);
    free(pargs);
    free(cargs);

    return 0;
}
