#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "miner_rush.h"
#include "pow.h"

typedef struct
{
    long start;
    long end;
    long target;
    long *solution;
} ThreadData;

static atomic_bool solved = false;
static volatile sig_atomic_t time_over = 0;
static volatile sig_atomic_t round_signal = 0;
static volatile sig_atomic_t winner_signal = 0;

static void *pow_worker(void *arg);

static void handle_alarm(int sig)
{
    (void)sig;
    time_over = 1;
    atomic_store(&solved, true);
}

static void handle_round(int sig)
{
    (void)sig;
    round_signal = 1;
}

static void handle_winner(int sig)
{
    (void)sig;
    winner_signal = 1;
    atomic_store(&solved, true);
}

static int parse_positive_int(const char *text, int *value)
{
    char *end = NULL;
    long parsed;

    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed <= 0 || parsed > INT32_MAX)
    {
        return -1;
    }

    *value = (int)parsed;
    return 0;
}

static void sleep_ms(long milliseconds)
{
    struct timespec req;

    if (milliseconds <= 0)
    {
        return;
    }

    req.tv_sec = milliseconds / 1000;
    req.tv_nsec = (milliseconds % 1000) * 1000000L;

    while (nanosleep(&req, &req) == -1 && errno == EINTR && !time_over)
    {
    }
}

static int setup_signal_handlers(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    sigfillset(&action.sa_mask);

    action.sa_handler = handle_alarm;
    if (sigaction(SIGALRM, &action, NULL) == -1)
    {
        perror("sigaction SIGALRM");
        return -1;
    }

    action.sa_handler = handle_round;
    if (sigaction(SIGUSR1, &action, NULL) == -1)
    {
        perror("sigaction SIGUSR1");
        return -1;
    }

    action.sa_handler = handle_winner;
    if (sigaction(SIGUSR2, &action, NULL) == -1)
    {
        perror("sigaction SIGUSR2");
        return -1;
    }

    return 0;
}

static int wait_sem(sem_t *sem)
{
    while (sem_wait(sem) == -1)
    {
        if (errno == EINTR)
        {
            if (time_over)
            {
                return -1;
            }
            continue;
        }
        perror("sem_wait");
        return -1;
    }

    return 0;
}

static int read_full(int fd, void *buffer, size_t size)
{
    char *ptr = buffer;
    size_t done = 0;

    while (done < size)
    {
        ssize_t n = read(fd, ptr + done, size - done);
        if (n == 0)
        {
            return 0;
        }
        if (n == -1)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return -1;
        }
        done += (size_t)n;
    }

    return 1;
}

static int write_full(int fd, const void *buffer, size_t size)
{
    const char *ptr = buffer;
    size_t done = 0;

    while (done < size)
    {
        ssize_t n = write(fd, ptr + done, size - done);
        if (n == -1)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return -1;
        }
        done += (size_t)n;
    }

    return 0;
}

static void logger_process(int read_fd, int write_fd)
{
    RushBlock message;
    int confirmation = 1;
    char filename[64];
    int file;
    pid_t parent_id = getppid();

    snprintf(filename, sizeof(filename), "%jd.log", (intmax_t)parent_id);
    file = open(filename, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (file == -1)
    {
        perror("open logger");
        exit(EXIT_FAILURE);
    }

    while (read_full(read_fd, &message, sizeof(message)) > 0)
    {
        if (message.type == RUSH_BLOCK_FINISH)
        {
            break;
        }

        dprintf(file, "Id: \t\t%d\n", message.round);
        dprintf(file, "Winner: \t%jd\n", (intmax_t)message.winner_pid);
        dprintf(file, "Target: \t%ld\n", message.target);
        if (message.is_valid)
        {
            dprintf(file, "Solution: \t%08ld (validated)\n", message.solution);
        }
        else
        {
            dprintf(file, "Solution: \t%08ld (rejected)\n", message.solution);
        }
        dprintf(file, "Votes: \t\t%d/%d\n", message.votes_yes, message.votes_total);
        dprintf(file, "Wallets: \t%jd:%d\n", (intmax_t)parent_id, message.coins);
        dprintf(file, "\n");

        if (write_full(write_fd, &confirmation, sizeof(confirmation)) == -1)
        {
            break;
        }
    }

    close(file);
    close(read_fd);
    close(write_fd);
    exit(EXIT_SUCCESS);
}

static int send_to_logger(int write_fd, int read_fd, const RushBlock *message)
{
    int confirmation;

    if (write_full(write_fd, message, sizeof(*message)) == -1)
    {
        return -1;
    }

    if (message->type == RUSH_BLOCK_FINISH)
    {
        return 0;
    }

    if (read_full(read_fd, &confirmation, sizeof(confirmation)) <= 0)
    {
        return -1;
    }

    return 0;
}

static RushShared *open_shared_memory(void)
{
    int fd_shm;
    RushShared *shared;

    fd_shm = shm_open(RUSH_SHM_NAME, O_RDWR, 0);
    if (fd_shm == -1)
    {
        fprintf(stderr, "Error: monitor must be started before miners\n");
        return NULL;
    }

    shared = mmap(NULL, sizeof(RushShared), PROT_READ | PROT_WRITE, MAP_SHARED, fd_shm, 0);
    close(fd_shm);
    if (shared == MAP_FAILED)
    {
        perror("mmap");
        return NULL;
    }

    return shared;
}

static mqd_t open_message_queue(void)
{
    mqd_t queue;

    queue = mq_open(RUSH_MQ_NAME, O_WRONLY);
    if (queue == (mqd_t)-1)
    {
        fprintf(stderr, "Error: monitor message queue is not available\n");
    }

    return queue;
}

static int find_miner_locked(RushShared *shared, pid_t pid)
{
    int i;

    for (i = 0; i < RUSH_MAX_MINERS; i++)
    {
        if (shared->miners[i].used && shared->miners[i].pid == pid)
        {
            return i;
        }
    }

    return -1;
}

static int find_free_slot_locked(RushShared *shared)
{
    int i;

    for (i = 0; i < RUSH_MAX_MINERS; i++)
    {
        if (!shared->miners[i].used)
        {
            return i;
        }
    }

    return -1;
}

static int count_miners_for_round_locked(RushShared *shared, int round)
{
    int i;
    int count = 0;

    for (i = 0; i < RUSH_MAX_MINERS; i++)
    {
        if (shared->miners[i].used && shared->miners[i].active &&
            shared->miners[i].joined_round <= round)
        {
            count++;
        }
    }

    return count;
}

static void start_new_round_locked(RushShared *shared)
{
    int i;

    shared->current_round++;
    shared->current_winner = 0;
    shared->current_winner_target = shared->current_target;
    shared->current_solution = -1;
    shared->winner_round = 0;
    shared->last_validation = 0;
    shared->votes_yes = 0;
    shared->votes_total = 0;

    for (i = 0; i < RUSH_MAX_MINERS; i++)
    {
        if (shared->miners[i].used && shared->miners[i].active &&
            shared->miners[i].joined_round <= shared->current_round)
        {
            shared->miners[i].vote_round = shared->current_round;
            shared->miners[i].vote = 0;
        }
    }
}

static int copy_active_pids(RushShared *shared, pid_t *pids, pid_t except_pid, int round)
{
    int i;
    int count = 0;

    if (wait_sem(&shared->state_mutex) == -1)
    {
        return 0;
    }

    for (i = 0; i < RUSH_MAX_MINERS; i++)
    {
        if (!shared->miners[i].used || !shared->miners[i].active)
        {
            continue;
        }
        if (except_pid > 0 && shared->miners[i].pid == except_pid)
        {
            continue;
        }
        if (round > 0 && shared->miners[i].joined_round > round)
        {
            continue;
        }
        pids[count++] = shared->miners[i].pid;
    }

    sem_post(&shared->state_mutex);
    return count;
}

static void signal_active_miners(RushShared *shared, int signal_number, pid_t except_pid, int round)
{
    pid_t pids[RUSH_MAX_MINERS];
    int count;
    int i;

    count = copy_active_pids(shared, pids, except_pid, round);
    for (i = 0; i < count; i++)
    {
        kill(pids[i], signal_number);
    }
}

static int monitor_is_alive(RushShared *shared)
{
    int alive;
    pid_t checker_pid;

    if (wait_sem(&shared->state_mutex) == -1)
    {
        return 0;
    }

    alive = shared->monitor_alive;
    checker_pid = shared->checker_pid;

    sem_post(&shared->state_mutex);

    if (!alive || checker_pid <= 0)
    {
        return 0;
    }

    if (kill(checker_pid, 0) == -1 && errno == ESRCH)
    {
        return 0;
    }

    return 1;
}

static int register_miner(RushShared *shared, pid_t pid, int *local_round, int *coins)
{
    int slot;
    int started_round = 0;

    if (wait_sem(&shared->state_mutex) == -1)
    {
        return -1;
    }

    if (!shared->monitor_alive)
    {
        sem_post(&shared->state_mutex);
        fprintf(stderr, "Error: monitor is not running\n");
        return -1;
    }

    slot = find_miner_locked(shared, pid);
    if (slot == -1)
    {
        slot = find_free_slot_locked(shared);
        if (slot == -1)
        {
            sem_post(&shared->state_mutex);
            fprintf(stderr, "Error: maximum number of miners reached\n");
            return -1;
        }
        memset(&shared->miners[slot], 0, sizeof(shared->miners[slot]));
        shared->miners[slot].used = 1;
        shared->miners[slot].pid = pid;
    }

    if (!shared->miners[slot].active)
    {
        shared->miners[slot].active = 1;
        shared->active_count++;
    }

    shared->miners[slot].joined_round = (shared->current_round == 0) ? 1 : shared->current_round + 1;
    shared->miners[slot].vote = 0;
    shared->miners[slot].vote_round = 0;

    if (shared->active_count >= 2 && shared->current_round == 0)
    {
        start_new_round_locked(shared);
        started_round = shared->current_round;
        *local_round = 0;
    }
    else
    {
        *local_round = shared->current_round;
    }

    *coins = shared->miners[slot].coins;

    sem_post(&shared->state_mutex);

    if (started_round > 0)
    {
        signal_active_miners(shared, SIGUSR1, 0, started_round);
    }

    return 0;
}

static int unregister_miner(RushShared *shared, pid_t pid)
{
    int slot;
    int is_last = 0;

    if (wait_sem(&shared->state_mutex) == -1)
    {
        return 0;
    }

    slot = find_miner_locked(shared, pid);
    if (slot != -1 && shared->miners[slot].active)
    {
        shared->miners[slot].active = 0;
        if (shared->active_count > 0)
        {
            shared->active_count--;
        }
    }

    is_last = (shared->active_count == 0);

    sem_post(&shared->state_mutex);
    return is_last;
}

static int get_next_round(RushShared *shared, pid_t pid, int local_round, int *round, long *target)
{
    int slot;
    int ready = 0;

    if (wait_sem(&shared->state_mutex) == -1)
    {
        return -1;
    }

    if (!shared->monitor_alive)
    {
        sem_post(&shared->state_mutex);
        return -1;
    }

    slot = find_miner_locked(shared, pid);
    if (slot == -1 || !shared->miners[slot].active)
    {
        sem_post(&shared->state_mutex);
        return -1;
    }

    if (shared->current_round > local_round &&
        shared->current_round >= shared->miners[slot].joined_round &&
        count_miners_for_round_locked(shared, shared->current_round) >= 2)
    {
        *round = shared->current_round;
        *target = shared->current_target;
        ready = 1;
    }

    sem_post(&shared->state_mutex);
    return ready;
}

static int run_pow_threads(int n_threads, long target, long *solution)
{
    pthread_t *threads;
    int created = 0;
    int i;
    long step = POW_LIMIT / n_threads;

    threads = calloc((size_t)n_threads, sizeof(*threads));
    if (threads == NULL)
    {
        perror("calloc");
        return -1;
    }

    atomic_store(&solved, false);
    *solution = -1;

    for (i = 0; i < n_threads; i++)
    {
        ThreadData *data = malloc(sizeof(*data));
        if (data == NULL)
        {
            perror("malloc");
            atomic_store(&solved, true);
            break;
        }

        data->start = i * step;
        data->end = (i == n_threads - 1) ? POW_LIMIT : (i + 1) * step;
        data->target = target;
        data->solution = solution;

        if (pthread_create(&threads[i], NULL, pow_worker, data) != 0)
        {
            perror("pthread_create");
            free(data);
            atomic_store(&solved, true);
            break;
        }
        created++;
    }

    for (i = 0; i < created; i++)
    {
        pthread_join(threads[i], NULL);
    }

    free(threads);
    return (created == n_threads) ? 0 : -1;
}

static int try_become_winner(RushShared *shared, pid_t pid, int round, long target, long solution)
{
    if (sem_trywait(&shared->winner_mutex) == -1)
    {
        if (errno != EAGAIN && errno != EINTR)
        {
            perror("sem_trywait");
        }
        return 0;
    }

    if (wait_sem(&shared->state_mutex) == -1)
    {
        sem_post(&shared->winner_mutex);
        return 0;
    }

    if (shared->current_round != round || shared->winner_round == round)
    {
        sem_post(&shared->state_mutex);
        sem_post(&shared->winner_mutex);
        return 0;
    }

    shared->winner_round = round;
    shared->current_winner = pid;
    shared->current_winner_target = target;
    shared->current_solution = solution;
    shared->votes_yes = 0;
    shared->votes_total = 0;

    sem_post(&shared->state_mutex);
    return 1;
}

static void count_votes(RushShared *shared, int round, pid_t winner_pid,
                        int *yes, int *total, int *expected)
{
    int i;

    *yes = 0;
    *total = 0;
    *expected = 0;

    for (i = 0; i < RUSH_MAX_MINERS; i++)
    {
        if (!shared->miners[i].used || !shared->miners[i].active ||
            shared->miners[i].pid == winner_pid ||
            shared->miners[i].joined_round > round)
        {
            continue;
        }

        (*expected)++;
        if (shared->miners[i].vote_round == round && shared->miners[i].vote != 0)
        {
            (*total)++;
            if (shared->miners[i].vote > 0)
            {
                (*yes)++;
            }
        }
    }
}

static void wait_for_votes(RushShared *shared, int round, pid_t winner_pid,
                           int *yes, int *total)
{
    int expected = 0;
    long waited = 0;

    while (!time_over && waited <= 3000)
    {
        if (wait_sem(&shared->state_mutex) == -1)
        {
            break;
        }

        count_votes(shared, round, winner_pid, yes, total, &expected);
        shared->votes_yes = *yes;
        shared->votes_total = *total;

        sem_post(&shared->state_mutex);

        if (*total >= expected)
        {
            break;
        }

        sleep_ms(100);
        waited += 100;
    }
}

static int store_vote(RushShared *shared, pid_t pid, int round, int vote,
                      int *yes, int *total)
{
    int slot;
    int expected;

    if (wait_sem(&shared->state_mutex) == -1)
    {
        return -1;
    }

    slot = find_miner_locked(shared, pid);
    if (slot == -1 || !shared->miners[slot].active)
    {
        sem_post(&shared->state_mutex);
        return -1;
    }

    shared->miners[slot].vote_round = round;
    shared->miners[slot].vote = vote ? 1 : -1;
    count_votes(shared, round, shared->current_winner, yes, total, &expected);
    shared->votes_yes = *yes;
    shared->votes_total = *total;

    sem_post(&shared->state_mutex);
    return 0;
}

static int get_winner_for_round(RushShared *shared, int round, pid_t *winner_pid,
                                long *winner_target, long *solution)
{
    long waited = 0;
    int found = 0;

    while (!time_over && waited <= 3000)
    {
        if (wait_sem(&shared->state_mutex) == -1)
        {
            return 0;
        }

        if (shared->winner_round == round)
        {
            *winner_pid = shared->current_winner;
            *winner_target = shared->current_winner_target;
            *solution = shared->current_solution;
            found = 1;
        }

        sem_post(&shared->state_mutex);

        if (found)
        {
            return 1;
        }

        sleep_ms(100);
        waited += 100;
    }

    return 0;
}

static int send_queue_block(mqd_t queue, const RushBlock *block)
{
    unsigned int priority = (block->type == RUSH_BLOCK_FINISH) ? 2 : 1;

    while (mq_send(queue, (const char *)block, sizeof(*block), priority) == -1)
    {
        if (errno == EINTR)
        {
            continue;
        }
        perror("mq_send");
        return -1;
    }

    return 0;
}

static void update_winner_state(RushShared *shared, pid_t pid, int round, long solution,
                                int accepted, int coins, int yes, int total,
                                int *next_round)
{
    if (wait_sem(&shared->state_mutex) == -1)
    {
        return;
    }

    if (accepted)
    {
        shared->current_target = solution;
    }

    {
        int slot = find_miner_locked(shared, pid);
        if (slot != -1)
        {
            shared->miners[slot].coins = coins;
        }
    }

    shared->votes_yes = yes;
    shared->votes_total = total;
    shared->last_validation = accepted;

    if (!time_over && shared->current_round == round &&
        count_miners_for_round_locked(shared, round + 1) >= 2)
    {
        start_new_round_locked(shared);
        *next_round = shared->current_round;
    }

    sem_post(&shared->state_mutex);
}

static void handle_winner_round(RushShared *shared, mqd_t queue, pid_t pid,
                                int logger_write, int logger_read, int round,
                                long target, long solution, int *coins)
{
    int yes = 0;
    int total = 0;
    int accepted;
    int next_round = 0;
    RushBlock block;

    signal_active_miners(shared, SIGUSR2, pid, round);
    wait_for_votes(shared, round, pid, &yes, &total);

    accepted = (pow_hash(solution) == target && (total == 0 || yes >= (total - yes)));
    if (accepted)
    {
        (*coins)++;
    }

    printf("Winner %jd => [", (intmax_t)pid);
    if (total > 0)
    {
        int i;
        for (i = 0; i < yes; i++)
        {
            printf(" Y");
        }
        for (i = yes; i < total; i++)
        {
            printf(" N");
        }
    }
    printf(" ] => %s\n", accepted ? "Accepted" : "Rejected");
    fflush(stdout);

    memset(&block, 0, sizeof(block));
    block.type = RUSH_BLOCK_RESULT;
    block.round = round;
    block.winner_pid = pid;
    block.target = target;
    block.solution = solution;
    block.is_valid = accepted;
    block.votes_yes = yes;
    block.votes_total = total;
    block.coins = *coins;

    send_to_logger(logger_write, logger_read, &block);
    send_queue_block(queue, &block);

    update_winner_state(shared, pid, round, solution, accepted, *coins, yes, total, &next_round);
    sem_post(&shared->winner_mutex);

    if (next_round > 0)
    {
        signal_active_miners(shared, SIGUSR1, 0, next_round);
    }
}

static void handle_voter_round(RushShared *shared, pid_t pid, int logger_write, int logger_read,
                               int round, long target, int coins)
{
    pid_t winner_pid = 0;
    long winner_target = target;
    long winner_solution = -1;
    int valid;
    int yes = 0;
    int total = 0;
    RushBlock block;

    if (!get_winner_for_round(shared, round, &winner_pid, &winner_target, &winner_solution))
    {
        return;
    }

    valid = (pow_hash(winner_solution) == winner_target);
    store_vote(shared, pid, round, valid, &yes, &total);

    memset(&block, 0, sizeof(block));
    block.type = RUSH_BLOCK_RESULT;
    block.round = round;
    block.winner_pid = winner_pid;
    block.target = winner_target;
    block.solution = winner_solution;
    block.is_valid = valid;
    block.votes_yes = yes;
    block.votes_total = total;
    block.coins = coins;

    send_to_logger(logger_write, logger_read, &block);
}

int main(int argc, char *argv[])
{
    int n_secs;
    int n_threads;
    int pipe_to_logger[2];
    int pipe_from_logger[2];
    pid_t logger_pid;
    RushShared *shared = NULL;
    mqd_t queue = (mqd_t)-1;
    pid_t my_pid = getpid();
    int local_round = 0;
    int coins = 0;
    int exit_status = EXIT_SUCCESS;

    if (argc != 3)
    {
        fprintf(stderr, "Usage: %s <N_SECS> <N_THREADS>\n", argv[0]);
        return EXIT_FAILURE;
    }

    if (parse_positive_int(argv[1], &n_secs) == -1 ||
        parse_positive_int(argv[2], &n_threads) == -1)
    {
        fprintf(stderr, "Error: arguments must be positive integers\n");
        return EXIT_FAILURE;
    }

    if (pipe(pipe_to_logger) == -1 || pipe(pipe_from_logger) == -1)
    {
        perror("pipe");
        return EXIT_FAILURE;
    }

    logger_pid = fork();
    if (logger_pid == -1)
    {
        perror("fork");
        return EXIT_FAILURE;
    }
    if (logger_pid == 0)
    {
        close(pipe_to_logger[1]);
        close(pipe_from_logger[0]);
        logger_process(pipe_to_logger[0], pipe_from_logger[1]);
    }

    close(pipe_to_logger[0]);
    close(pipe_from_logger[1]);

    if (setup_signal_handlers() == -1)
    {
        exit_status = EXIT_FAILURE;
        goto finish_logger;
    }

    shared = open_shared_memory();
    if (shared == NULL)
    {
        exit_status = EXIT_FAILURE;
        goto finish_logger;
    }

    queue = open_message_queue();
    if (queue == (mqd_t)-1)
    {
        exit_status = EXIT_FAILURE;
        goto cleanup_shared;
    }

    if (!monitor_is_alive(shared))
    {
        fprintf(stderr, "Error: monitor is not running\n");
        exit_status = EXIT_FAILURE;
        goto cleanup_queue;
    }

    if (register_miner(shared, my_pid, &local_round, &coins) == -1)
    {
        exit_status = EXIT_FAILURE;
        goto cleanup_queue;
    }

    alarm((unsigned int)n_secs);

    while (!time_over)
    {
        int round;
        long target;
        int ready;
        long solution = -1;

        if (!monitor_is_alive(shared))
        {
            fprintf(stderr, "Monitor stopped. Miner %jd finishing.\n", (intmax_t)my_pid);
            break;
        }

        ready = get_next_round(shared, my_pid, local_round, &round, &target);
        if (ready == -1)
        {
            break;
        }
        if (!ready)
        {
            sleep_ms(100);
            continue;
        }

        local_round = round;
        round_signal = 0;
        winner_signal = 0;

        if (run_pow_threads(n_threads, target, &solution) == -1)
        {
            exit_status = EXIT_FAILURE;
            break;
        }

        if (solution != -1 && try_become_winner(shared, my_pid, round, target, solution))
        {
            handle_winner_round(shared, queue, my_pid, pipe_to_logger[1], pipe_from_logger[0],
                                round, target, solution, &coins);
        }
        else
        {
            handle_voter_round(shared, my_pid, pipe_to_logger[1], pipe_from_logger[0],
                               round, target, coins);
        }
    }

    if (shared != NULL)
    {
        int is_last = unregister_miner(shared, my_pid);
        if (is_last && queue != (mqd_t)-1 && monitor_is_alive(shared))
        {
            RushBlock finish_block;
            memset(&finish_block, 0, sizeof(finish_block));
            finish_block.type = RUSH_BLOCK_FINISH;
            send_queue_block(queue, &finish_block);
        }
    }

cleanup_queue:
    if (queue != (mqd_t)-1)
    {
        mq_close(queue);
    }

cleanup_shared:
    if (shared != NULL)
    {
        munmap(shared, sizeof(RushShared));
    }

finish_logger:
    {
        RushBlock finish_block;
        int status;

        memset(&finish_block, 0, sizeof(finish_block));
        finish_block.type = RUSH_BLOCK_FINISH;
        send_to_logger(pipe_to_logger[1], pipe_from_logger[0], &finish_block);

        close(pipe_to_logger[1]);
        close(pipe_from_logger[0]);
        waitpid(logger_pid, &status, 0);
    }

    return exit_status;
}

static void *pow_worker(void *arg)
{
    ThreadData *data = arg;
    long i;

    for (i = data->start; i < data->end && !atomic_load(&solved); i++)
    {
        if (pow_hash(i) == data->target)
        {
            if (!atomic_exchange(&solved, true))
            {
                *data->solution = i;
            }
            break;
        }
    }

    free(data);
    return NULL;
}
