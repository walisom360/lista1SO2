// race_bet_fixed.c
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

#define N_HORSES 10
#define FINISH_SCORE 100

typedef struct {
    int id;
    uint32_t rng;
    double mean_service_ms;
    int debug;
} Horse;

/* RNG xorshift32 (por-horse) */
static uint32_t xorshift32(uint32_t *state) {
    uint32_t x = *state;
    if (x == 0) x = 0xDEADBEEF;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/* distribuidor -> avanço 1..10 */
static int distributor_advance(Horse *h) {
    uint32_t r = xorshift32(&h->rng);
    return (r % 10) + 1; // 1..10
}

/* --- barrier reutilizável (implementada com mutex + cond) --- */
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int count;          // quantas threads já chegaram
    int trip_count;     // quantas threads devem chegar para liberar
    int generation;     // gerações (para reutilização)
} reusable_barrier_t;

static void barrier_init(reusable_barrier_t *b, int trip_count) {
    pthread_mutex_init(&b->mutex, NULL);
    pthread_cond_init(&b->cond, NULL);
    b->count = 0;
    b->trip_count = trip_count;
    b->generation = 0;
}

/* Espera na barreira; se race_over for acionado externamente, o caller
   pode acordar as threads fazendo generation++ e broadcast via helper abaixo. */
static int barrier_wait(reusable_barrier_t *b, int *race_over_flag) {
    pthread_mutex_lock(&b->mutex);
    int my_gen = b->generation;
    b->count++;
    if (b->count >= b->trip_count) {
        /* última thread a chegar libera a geração */
        b->generation++;
        b->count = 0;
        pthread_cond_broadcast(&b->cond);
        pthread_mutex_unlock(&b->mutex);
        return 1; // esta thread foi a "serial" (opcional)
    } else {
        /* espera até que a geração mude (liberada) ou até race_over */
        while (my_gen == b->generation && !(*race_over_flag)) {
            pthread_cond_wait(&b->cond, &b->mutex);
        }
        pthread_mutex_unlock(&b->mutex);
        return 0;
    }
}

/* Força liberar todas as threads que estão esperando na barreira (útil quando a corrida termina). */
static void barrier_force_release(reusable_barrier_t *b) {
    pthread_mutex_lock(&b->mutex);
    b->generation++; // muda geração para acordar os que estiverem esperando
    b->count = 0;
    pthread_cond_broadcast(&b->cond);
    pthread_mutex_unlock(&b->mutex);
}

static void barrier_destroy(reusable_barrier_t *b) {
    pthread_mutex_destroy(&b->mutex);
    pthread_cond_destroy(&b->cond);
}

/* --- estado global da corrida --- */
static int scores[N_HORSES];
static pthread_mutex_t score_mutex = PTHREAD_MUTEX_INITIALIZER;

static int winner = -1;                  // id do vencedor final (0..9)
static int winner_score = -1;            // pontuação do vencedor
static pthread_mutex_t winner_mutex = PTHREAD_MUTEX_INITIALIZER;

static int race_over = 0;                // flag indicando fim da corrida
static pthread_mutex_t race_over_mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t print_mutex = PTHREAD_MUTEX_INITIALIZER;

/* start_barrier: main + N_HORSES para largada sincronizada */
static reusable_barrier_t start_barrier;
/* round_barrier: as N_HORSES threads sincronizam a cada rodada */
static reusable_barrier_t round_barrier;

/* Checa e seta race_over de forma atômica. Se setou, força liberar barreiras. */
static void set_race_over_and_release() {
    pthread_mutex_lock(&race_over_mutex);
    race_over = 1;
    pthread_mutex_unlock(&race_over_mutex);

    /* garante que qualquer thread esperando em qualquer barrier seja acordada */
    barrier_force_release(&start_barrier);
    barrier_force_release(&round_barrier);
}

void *horse_thread(void *arg) {
    Horse *h = (Horse *)arg;

    /* espera largada sincronizada (com main). start_barrier.trip_count == N_HORSES + 1 */
    barrier_wait(&start_barrier, &race_over);

    while (1) {
        /* sincroniza começo da rodada com os outros cavalos */
        barrier_wait(&round_barrier, &race_over);

        /* checa se corrida já acabou (outro cavalo venceu e forçou liberação) */
        pthread_mutex_lock(&race_over_mutex);
        if (race_over) {
            pthread_mutex_unlock(&race_over_mutex);
            break;
        }
        pthread_mutex_unlock(&race_over_mutex);

        /* pega avanço do distribuidor */
        int adv = distributor_advance(h);

        /* atualiza placar com exclusão mútua */
        pthread_mutex_lock(&score_mutex);
        scores[h->id] += adv;
        int my_score = scores[h->id];
        pthread_mutex_unlock(&score_mutex);

        /* imprime de forma segura */
        pthread_mutex_lock(&print_mutex);
        printf("Cavalo %d: +%d (total=%d)\n", h->id + 1, adv, my_score);
        pthread_mutex_unlock(&print_mutex);

        /* verifica/atualiza vencedor de forma atômica e determinística */
        int declare_winner = 0;
        pthread_mutex_lock(&winner_mutex);
        if (my_score >= FINISH_SCORE) {
            if (winner == -1) {
                /* primeiro a ultrapassar */
                winner = h->id;
                winner_score = my_score;
                declare_winner = 1;
            } else {
                /* existe candidato: desempate determinístico */
                if (my_score > winner_score ||
                    (my_score == winner_score && h->id < winner)) {
                    winner = h->id;
                    winner_score = my_score;
                    declare_winner = 1;
                }
            }
        }
        pthread_mutex_unlock(&winner_mutex);

        if (declare_winner) {
            pthread_mutex_lock(&print_mutex);
            printf(">>> Cavalo %d ultrapassou %d pontos e está na frente (score=%d)\n", winner + 1, FINISH_SCORE, winner_score);
            pthread_mutex_unlock(&print_mutex);

            /* marca corrida como terminada e força liberação das barreiras */
            set_race_over_and_release();
            break;
        }

        /* pequeno delay para simular tempo de serviço (opcional) */
        if (h->mean_service_ms > 0) {
            usleep((useconds_t)(h->mean_service_ms * 1000));
        }

        /* volta para próxima rodada */
    }

    int *ret = malloc(sizeof(int));
    *ret = h->id;
    return ret;
}

int main(void) {
    pthread_t threads[N_HORSES];
    Horse horses[N_HORSES];

    /* inicializa scores */
    for (int i = 0; i < N_HORSES; ++i) scores[i] = 0;

    /* inicializa barriers:
       - start_barrier com N_HORSES + 1 (o main participa para liberar a largada)
       - round_barrier com N_HORSES (apenas os cavalos sincronizam por rodada)
    */
    barrier_init(&start_barrier, N_HORSES + 1);
    barrier_init(&round_barrier, N_HORSES);

    /* pergunta a aposta para o usuário (1..10) */
    int bet = 0;
    do {
        printf("Aposte em um cavalo (1-%d): ", N_HORSES);
        if (scanf("%d", &bet) != 1) {
            /* limpar stdin em caso de erro e tentar de novo */
            int c;
            while ((c = getchar()) != '\n' && c != EOF) {}
            bet = 0;
            continue;
        }
        if (bet < 1 || bet > N_HORSES) {
            printf("Entrada inválida. Escolha um número entre 1 e %d.\n", N_HORSES);
        }
    } while (bet < 1 || bet > N_HORSES);

    /* cria threads */
    srand((unsigned)time(NULL));
    for (int i = 0; i < N_HORSES; ++i) {
        horses[i].id = i;
        horses[i].rng = (uint32_t)rand() ^ (uint32_t)(i * 0x9e3779b9);
        horses[i].mean_service_ms = 20 + (rand() % 50); // delay opcional
        horses[i].debug = 0;
        if (pthread_create(&threads[i], NULL, horse_thread, &horses[i]) != 0) {
            perror("pthread_create");
            return 1;
        }
    }

    /* largada sincronizada: main participa da barrier para liberar todas ao mesmo tempo */
    printf("\nPreparar...\n");
    sleep(1);
    printf("Prontos...\n");
    sleep(1);
    printf("LARGADA!\n\n");
    barrier_wait(&start_barrier, &race_over);

    /* espera as threads terminarem (join) */
    for (int i = 0; i < N_HORSES; ++i) {
        void *res;
        if (pthread_join(threads[i], &res) != 0) {
            perror("pthread_join");
        } else {
            if (res) free(res);
        }
    }

    /* se por algum motivo race_over não foi setado (improvável), força liberação e checa vencedor */
    pthread_mutex_lock(&race_over_mutex);
    if (!race_over) {
        race_over = 1;
        barrier_force_release(&round_barrier);
    }
    pthread_mutex_unlock(&race_over_mutex);

    /* destrói sincronizadores */
    barrier_destroy(&start_barrier);
    barrier_destroy(&round_barrier);
    pthread_mutex_destroy(&score_mutex);
    pthread_mutex_destroy(&winner_mutex);
    pthread_mutex_destroy(&race_over_mutex);
    pthread_mutex_destroy(&print_mutex);

    /* resultado final */
    printf("\n--- Resultado final ---\n");
    for (int i = 0; i < N_HORSES; ++i) {
        printf("Cavalo %d -> %d\n", i + 1, scores[i]);
    }

    if (winner != -1) {
        printf("\nVencedor: Cavalo %d (pontuação = %d)\n", winner + 1, winner_score);
        if (bet == winner + 1) {
            printf("Parabéns! Sua aposta (%d) foi correta.\n", bet);
        } else {
            printf("Sua aposta (%d) NÃO venceu. Melhor sorte da próxima vez.\n", bet);
        }
    } else {
        printf("\nNenhum vencedor detectado (improvável).\n");
    }

    return 0;
}
