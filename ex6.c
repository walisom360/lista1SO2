// par_sumhist.c
// Soma total e histograma com P threads em paralelo.
// - Particiona arquivo mapeado em memória (mmap) em P blocos por byte-range.
// - Cada thread faz "map" local: soma parcial + histograma local.
// - A principal faz "reduce" (merge) sem mutex após join.
// - Mede tempo (ms) para calcular speedup rodando P=1,2,4,8.
//
// Formato do arquivo: inteiros (com sinal opcional) separados por espaços/linhas.
// Histograma cobre intervalo [MIN, MAX). Fora desse intervalo: entram na soma, não no histograma.

#define _GNU_SOURCE
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <time.h>

/* >>>>>> ALTERE AQUI O CAMINHO PADRÃO DO ARQUIVO DE ENTRADA <<<<<<
   Ex.: "dataset_10k.txt" (mesma pasta do executável) ou caminho absoluto. */
#ifndef DEFAULT_INPUT_PATH
#define DEFAULT_INPUT_PATH "dataset_10k.txt"
#endif

typedef struct {
    const char *base;     // mmap base
    size_t start;         // início do bloco (ajustado p/ fronteira)
    size_t end;           // fim do bloco (exclusivo, ajustado)
    int64_t local_sum;    // soma parcial
    uint64_t *local_hist; // histograma local (bins = MAX-MIN)
    long long nints;      // inteiros contados
    int MIN, MAX;         // faixa hist
} Worker;

static inline uint64_t now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec*1000ULL + (uint64_t)ts.tv_nsec/1000000ULL;
}

static inline int isdelim(char c) {
    return isspace((unsigned char)c);
}

typedef struct { int P; int MIN; int MAX; const char *file; int print_hist; int quiet; } Args;

static void usage(const char *p) {
    fprintf(stderr,
      "Uso: %s [-f <arquivo>] [-p P] [-L MIN] [-U MAX] [-H] [-q]\n"
      "  -f arquivo : caminho do arquivo de inteiros (padrao: %s)\n"
      "  -p P       : numero de threads (default 4)\n"
      "  -L MIN     : menor valor do histograma (default 0)\n"
      "  -U MAX     : limite superior exclusivo do histograma (default 10000)\n"
      "  -H         : imprime histograma completo (valor -> contagem)\n"
      "  -q         : silencioso (nao imprime histograma)\n",
      p, DEFAULT_INPUT_PATH);
}

static bool parse_args(int argc, char **argv, Args *a) {
    a->P = 4; a->MIN = 0; a->MAX = 10000; a->file = DEFAULT_INPUT_PATH; a->print_hist = 0; a->quiet = 0;
    int opt;
    while ((opt = getopt(argc, argv, "f:p:L:U:Hq")) != -1) {
        switch (opt) {
            case 'f': a->file = optarg; break;
            case 'p': a->P = atoi(optarg); break;
            case 'L': a->MIN = atoi(optarg); break;
            case 'U': a->MAX = atoi(optarg); break;
            case 'H': a->print_hist = 1; break;
            case 'q': a->quiet = 1; break;
            default: usage(argv[0]); return false;
        }
    }
    if (a->P <= 0 || a->MIN >= a->MAX) { usage(argv[0]); return false; }
    return true;
}

// Ajusta [start,end) para que comecem/terminem em fronteiras de token.
// Estratégia: cada thread t recebe um range bruto; t>0 avança start até próximo delimitador.
// Para end: avança até próximo delimitador (inclui o número corrente); evita duplo processamento.
static void align_block(const char *base, size_t fsz, size_t *start, size_t *end, int is_first, int is_last) {
    size_t s = *start, e = *end;
    if (!is_first) {
        // Se começar no meio de um número, avance até encontrar um delimitador
        while (s < fsz && !isdelim(base[s])) s++;
        // Pula delimitadores
        while (s < fsz && isdelim(base[s])) s++;
    }
    if (!is_last) {
        // Avança o fim até o próximo delimitador (para consumir o número corrente por esta thread)
        if (e < fsz) {
            while (e < fsz && !isdelim(base[e])) e++;
        }
    } else {
        e = fsz; // última thread vai até o fim
    }
    if (s > e) s = e;
    *start = s; *end = e;
}

static void *worker_fn(void *arg) {
    Worker *w = (Worker*)arg;
    const char *p = w->base + w->start;
    const char *endp = w->base + w->end;
    int64_t sum = 0;
    long long cnt = 0;

    while (p < endp) {
        // pular espaços
        while (p < endp && isdelim(*p)) p++;
        if (p >= endp) break;

        // parse opcional sinal
        int sign = 1;
        if (*p == '+' || *p == '-') {
            if (*p == '-') sign = -1;
            p++;
            if (p >= endp) break;
        }

        // precisa de pelo menos um dígito
        if (p < endp && isdigit((unsigned char)*p)) {
            int64_t val = 0;
            while (p < endp && isdigit((unsigned char)*p)) {
                val = val * 10 + (*p - '0');
                p++;
            }
            val *= sign;
            sum += val;
            cnt++;

            // hist local
            if (val >= w->MIN && val < w->MAX) {
                w->local_hist[(size_t)(val - w->MIN)]++;
            }
        } else {
            // caractere inesperado: avance 1 (tolera lixo)
            p++;
        }
    }

    w->local_sum = sum;
    w->nints = cnt;
    return NULL;
}

int main(int argc, char **argv) {
    Args a;
    if (!parse_args(argc, argv, &a)) return 1;

    // Abrir e mapear arquivo
    int fd = open(a.file, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "Erro ao abrir '%s': %s\n", a.file, strerror(errno));
        fprintf(stderr, "Dica: ajuste DEFAULT_INPUT_PATH no codigo ou passe -f <arquivo>.\n");
        return 1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) { perror("fstat"); close(fd); return 1; }
    size_t fsz = (size_t)st.st_size;
    if (fsz == 0) { fprintf(stderr, "Arquivo vazio.\n"); close(fd); return 1; }

    void *map = mmap(NULL, fsz, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) { perror("mmap"); close(fd); return 1; }
    const char *base = (const char*)map;

    int P = a.P;
    size_t bins = (size_t)(a.MAX - a.MIN);

    // Aloca workers
    Worker *w = (Worker*)calloc((size_t)P, sizeof(Worker));
    pthread_t *threads = (pthread_t*)malloc(sizeof(pthread_t)*(size_t)P);
    if (!w || !threads) { fprintf(stderr, "alloc failed\n"); munmap(map, fsz); close(fd); return 1; }

    // Aloca histogramas locais
    for (int i = 0; i < P; ++i) {
        w[i].local_hist = (uint64_t*)calloc(bins, sizeof(uint64_t));
        if (!w[i].local_hist) { fprintf(stderr, "alloc hist failed\n"); return 1; }
    }

    // Particiona ranges e alinha fronteiras
    for (int i = 0; i < P; ++i) {
        size_t raw_s = (size_t)((__uint128_t)i * fsz / (unsigned)P);
        size_t raw_e = (size_t)((__uint128_t)(i+1) * fsz / (unsigned)P);
        w[i].base = base;
        w[i].start = raw_s;
        w[i].end = raw_e;
        w[i].MIN = a.MIN; w[i].MAX = a.MAX;
        align_block(base, fsz, &w[i].start, &w[i].end, i==0, i==(P-1));
    }

    uint64_t t0 = now_ms();

    // Cria threads
    for (int i = 0; i < P; ++i) {
        if (pthread_create(&threads[i], NULL, worker_fn, &w[i]) != 0) {
            fprintf(stderr, "pthread_create: %s\n", strerror(errno)); return 2;
        }
    }
    // Aguarda
    for (int i = 0; i < P; ++i) pthread_join(threads[i], NULL);

    // Reduce na principal
    int64_t total_sum = 0;
    long long total_count = 0;
    uint64_t *global_hist = (uint64_t*)calloc(bins, sizeof(uint64_t));
    if (!global_hist) { fprintf(stderr, "alloc global hist failed\n"); return 1; }

    for (int i = 0; i < P; ++i) {
        total_sum += w[i].local_sum;
        total_count += w[i].nints;
        // merge hist
        for (size_t b = 0; b < bins; ++b) {
            global_hist[b] += w[i].local_hist[b];
        }
    }

    uint64_t t1 = now_ms();

    // Impressões
    printf("Arquivo: %s\n", a.file);
    printf("Threads: %d\n", P);
    printf("Faixa hist: [%d, %d)\n", a.MIN, a.MAX);
    printf("Inteiros lidos: %lld\n", total_count);
    printf("Soma total: %" PRId64 "\n", total_sum);
    printf("Tempo: %" PRIu64 " ms\n", (t1 - t0));

    if (!a.quiet) {
        // Conta bins nao-vazios
        size_t nonzero = 0;
        for (size_t b = 0; b < bins; ++b) if (global_hist[b] != 0) nonzero++;
        printf("Bins nao-vazios: %zu de %zu\n", nonzero, bins);

        if (a.print_hist) {
            for (int v = a.MIN; v < a.MAX; ++v) {
                uint64_t c = global_hist[(size_t)(v - a.MIN)];
                if (c) printf("%d %llu\n", v, (unsigned long long)c);
            }
        }
    }

    // Limpeza
    for (int i = 0; i < P; ++i) free(w[i].local_hist);
    free(global_hist);
    free(threads);
    free(w);
    munmap((void*)base, fsz);
    close(fd);
    return 0;
}
