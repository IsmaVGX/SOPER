#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <signal.h>
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

static volatile sig_atomic_t stop_requested = 0;

static void handle_stop(int sig)
{
    (void)sig;
    stop_requested = 1;
}

static int parse_lag(const char *text, long *value)
{
    char *end = NULL;
    long parsed;

    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed < 0)
    {
        return -1;
    }

    *value = parsed;
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

    while (nanosleep(&req, &req) == -1 && errno == EINTR && !stop_requested)
    {
    }
}

static int setup_signal_handlers(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_stop;
    sigfillset(&action.sa_mask);

    if (sigaction(SIGINT, &action, NULL) == -1)
    {
        perror("sigaction");
        return -1;
    }
    if (sigaction(SIGTERM, &action, NULL) == -1)
    {
        perror("sigaction");
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
            if (stop_requested)
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

static int put_block(RushShared *shared, const RushBlock *block)
{
    if (wait_sem(&shared->sem_empty) == -1)
    {
        return -1;
    }
    if (wait_sem(&shared->sem_mutex) == -1)
    {
        sem_post(&shared->sem_empty);
        return -1;
    }

    shared->buffer[shared->buffer_in] = *block;
    shared->buffer_in = (shared->buffer_in + 1) % RUSH_BUFFER_SIZE;

    sem_post(&shared->sem_mutex);
    sem_post(&shared->sem_fill);

    return 0;
}

static int get_block(RushShared *shared, RushBlock *block)
{
    if (wait_sem(&shared->sem_fill) == -1)
    {
        return -1;
    }
    if (wait_sem(&shared->sem_mutex) == -1)
    {
        sem_post(&shared->sem_fill);
        return -1;
    }

    *block = shared->buffer[shared->buffer_out];
    shared->buffer_out = (shared->buffer_out + 1) % RUSH_BUFFER_SIZE;

    sem_post(&shared->sem_mutex);
    sem_post(&shared->sem_empty);

    return 0;
}

static int init_shared_memory(RushShared *shared)
{
    memset(shared, 0, sizeof(*shared));

    if (sem_init(&shared->sem_empty, 1, RUSH_BUFFER_SIZE) == -1)
    {
        perror("sem_init sem_empty");
        return -1;
    }
    if (sem_init(&shared->sem_fill, 1, 0) == -1)
    {
        perror("sem_init sem_fill");
        sem_destroy(&shared->sem_empty);
        return -1;
    }
    if (sem_init(&shared->sem_mutex, 1, 1) == -1)
    {
        perror("sem_init sem_mutex");
        sem_destroy(&shared->sem_fill);
        sem_destroy(&shared->sem_empty);
        return -1;
    }
    if (sem_init(&shared->state_mutex, 1, 1) == -1)
    {
        perror("sem_init state_mutex");
        sem_destroy(&shared->sem_mutex);
        sem_destroy(&shared->sem_fill);
        sem_destroy(&shared->sem_empty);
        return -1;
    }
    if (sem_init(&shared->winner_mutex, 1, 1) == -1)
    {
        perror("sem_init winner_mutex");
        sem_destroy(&shared->state_mutex);
        sem_destroy(&shared->sem_mutex);
        sem_destroy(&shared->sem_fill);
        sem_destroy(&shared->sem_empty);
        return -1;
    }

    shared->monitor_alive = 1;
    shared->checker_pid = getpid();
    shared->current_target = RUSH_INITIAL_TARGET;
    shared->current_winner_target = RUSH_INITIAL_TARGET;
    shared->current_solution = -1;

    return 0;
}

static void destroy_shared_memory(RushShared *shared)
{
    sem_destroy(&shared->winner_mutex);
    sem_destroy(&shared->state_mutex);
    sem_destroy(&shared->sem_mutex);
    sem_destroy(&shared->sem_fill);
    sem_destroy(&shared->sem_empty);
}

static int shared_memory_is_stale(void)
{
    int fd_shm;
    RushShared *shared;
    int alive;
    pid_t checker_pid;

    fd_shm = shm_open(RUSH_SHM_NAME, O_RDWR, 0);
    if (fd_shm == -1)
    {
        return 1;
    }

    shared = mmap(NULL, sizeof(RushShared), PROT_READ | PROT_WRITE, MAP_SHARED, fd_shm, 0);
    close(fd_shm);
    if (shared == MAP_FAILED)
    {
        return 1;
    }

    alive = shared->monitor_alive;
    checker_pid = shared->checker_pid;
    munmap(shared, sizeof(RushShared));

    if (!alive || checker_pid <= 0)
    {
        return 1;
    }
    if (kill(checker_pid, 0) == -1 && errno == ESRCH)
    {
        return 1;
    }

    return 0;
}

static RushShared *create_shared_memory(int *fd_shm)
{
    RushShared *shared;
    int attempts;

    for (attempts = 0; attempts < 2; attempts++)
    {
        *fd_shm = shm_open(RUSH_SHM_NAME, O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR);
        if (*fd_shm != -1)
        {
            break;
        }

        if (errno == EEXIST && attempts == 0 && shared_memory_is_stale())
        {
            shm_unlink(RUSH_SHM_NAME);
            continue;
        }

        if (errno == EEXIST)
        {
            fprintf(stderr, "Error: shared memory already exists. Is monitor already running?\n");
        }
        else
        {
            perror("shm_open");
        }
        return NULL;
    }

    if (ftruncate(*fd_shm, sizeof(RushShared)) == -1)
    {
        perror("ftruncate");
        close(*fd_shm);
        shm_unlink(RUSH_SHM_NAME);
        return NULL;
    }

    shared = mmap(NULL, sizeof(RushShared), PROT_READ | PROT_WRITE, MAP_SHARED, *fd_shm, 0);
    if (shared == MAP_FAILED)
    {
        perror("mmap");
        close(*fd_shm);
        shm_unlink(RUSH_SHM_NAME);
        return NULL;
    }

    if (init_shared_memory(shared) == -1)
    {
        munmap(shared, sizeof(RushShared));
        close(*fd_shm);
        shm_unlink(RUSH_SHM_NAME);
        return NULL;
    }

    return shared;
}

static mqd_t create_message_queue(void)
{
    struct mq_attr attributes;
    mqd_t queue;
    int attempts;

    memset(&attributes, 0, sizeof(attributes));
    attributes.mq_maxmsg = RUSH_MQ_MAXMSG;
    attributes.mq_msgsize = sizeof(RushBlock);

    for (attempts = 0; attempts < 2; attempts++)
    {
        queue = mq_open(RUSH_MQ_NAME, O_CREAT | O_EXCL | O_RDONLY, S_IRUSR | S_IWUSR, &attributes);
        if (queue != (mqd_t)-1)
        {
            return queue;
        }

        if (errno == EEXIST && attempts == 0)
        {
            mq_unlink(RUSH_MQ_NAME);
            continue;
        }

        if (errno == EEXIST)
        {
            fprintf(stderr, "Error: message queue already exists. Is monitor already running?\n");
        }
        else
        {
            perror("mq_open");
        }
    }

    return (mqd_t)-1;
}

static RushShared *open_shared_memory(void)
{
    int fd_shm;
    RushShared *shared;

    fd_shm = shm_open(RUSH_SHM_NAME, O_RDWR, 0);
    if (fd_shm == -1)
    {
        perror("shm_open");
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

static void mark_monitor_stopped(RushShared *shared)
{
    if (wait_sem(&shared->state_mutex) == -1)
    {
        return;
    }

    shared->monitor_alive = 0;

    sem_post(&shared->state_mutex);
}

static void update_checked_block(RushShared *shared, const RushBlock *block)
{
    int i;

    if (wait_sem(&shared->state_mutex) == -1)
    {
        return;
    }

    shared->current_solution = block->solution;
    shared->current_winner = block->winner_pid;
    shared->last_validation = block->is_valid;
    shared->votes_yes = block->votes_yes;
    shared->votes_total = block->votes_total;

    for (i = 0; i < RUSH_MAX_MINERS; i++)
    {
        if (shared->miners[i].used && shared->miners[i].pid == block->winner_pid)
        {
            shared->miners[i].coins = block->coins;
            break;
        }
    }

    sem_post(&shared->state_mutex);
}

static void checker_loop(RushShared *shared, mqd_t queue, long lag_checker)
{
    RushBlock block;
    ssize_t received;

    while (!stop_requested)
    {
        received = mq_receive(queue, (char *)&block, sizeof(block), NULL);
        if (received == -1)
        {
            if (errno == EINTR)
            {
                continue;
            }
            perror("mq_receive");
            break;
        }

        if ((size_t)received != sizeof(block))
        {
            fprintf(stderr, "Warning: incomplete block received\n");
            continue;
        }

        if (block.type == RUSH_BLOCK_FINISH)
        {
            put_block(shared, &block);
            break;
        }

        block.is_valid = (pow_hash(block.solution) == block.target);
        update_checked_block(shared, &block);

        if (put_block(shared, &block) == -1)
        {
            break;
        }

        sleep_ms(lag_checker);
    }

    if (stop_requested)
    {
        memset(&block, 0, sizeof(block));
        block.type = RUSH_BLOCK_FINISH;
        put_block(shared, &block);
    }
}

static int monitor_loop(long lag_monitor)
{
    RushShared *shared;
    RushBlock block;

    shared = open_shared_memory();
    if (shared == NULL)
    {
        return EXIT_FAILURE;
    }

    printf("[%jd] Printing blocks ...\n", (intmax_t)getpid());
    fflush(stdout);

    while (!stop_requested)
    {
        if (get_block(shared, &block) == -1)
        {
            break;
        }

        if (block.type == RUSH_BLOCK_FINISH)
        {
            break;
        }

        if (block.is_valid)
        {
            printf("Solution accepted: %08ld --> %08ld\n", block.target, block.solution);
        }
        else
        {
            printf("Solution rejected: %08ld !-> %08ld\n", block.target, block.solution);
        }
        fflush(stdout);

        sleep_ms(lag_monitor);
    }

    printf("[%jd] Finishing\n", (intmax_t)getpid());
    fflush(stdout);

    munmap(shared, sizeof(RushShared));
    return EXIT_SUCCESS;
}

int main(int argc, char *argv[])
{
    long lag_checker;
    long lag_monitor;
    int fd_shm = -1;
    RushShared *shared = NULL;
    mqd_t queue = (mqd_t)-1;
    pid_t child;
    int status = EXIT_SUCCESS;

    if (argc != 3)
    {
        fprintf(stderr, "Usage: %s <LAG_COMPROBADOR> <LAG_MONITOR>\n", argv[0]);
        return EXIT_FAILURE;
    }

    if (parse_lag(argv[1], &lag_checker) == -1 || parse_lag(argv[2], &lag_monitor) == -1)
    {
        fprintf(stderr, "Error: lags must be non-negative integers in milliseconds\n");
        return EXIT_FAILURE;
    }

    if (setup_signal_handlers() == -1)
    {
        return EXIT_FAILURE;
    }

    shared = create_shared_memory(&fd_shm);
    if (shared == NULL)
    {
        return EXIT_FAILURE;
    }

    queue = create_message_queue();
    if (queue == (mqd_t)-1)
    {
        destroy_shared_memory(shared);
        munmap(shared, sizeof(RushShared));
        close(fd_shm);
        shm_unlink(RUSH_SHM_NAME);
        return EXIT_FAILURE;
    }

    child = fork();
    if (child == -1)
    {
        perror("fork");
        status = EXIT_FAILURE;
    }
    else if (child == 0)
    {
        mq_close(queue);
        munmap(shared, sizeof(RushShared));
        close(fd_shm);
        return monitor_loop(lag_monitor);
    }
    else
    {
        if (wait_sem(&shared->state_mutex) == -1)
        {
            status = EXIT_FAILURE;
        }
        else
        {
            shared->monitor_pid = child;
            sem_post(&shared->state_mutex);
            checker_loop(shared, queue, lag_checker);
            mark_monitor_stopped(shared);
            waitpid(child, NULL, 0);
        }
    }

    destroy_shared_memory(shared);
    munmap(shared, sizeof(RushShared));
    close(fd_shm);
    mq_close(queue);
    mq_unlink(RUSH_MQ_NAME);
    shm_unlink(RUSH_SHM_NAME);

    return status;
}
