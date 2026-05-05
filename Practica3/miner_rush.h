#ifndef MINER_RUSH_H
#define MINER_RUSH_H

#include <semaphore.h>
#include <sys/types.h>

#define RUSH_SHM_NAME "/soper_miner_rush_shm"
#define RUSH_MQ_NAME "/soper_miner_rush_mq"

#define RUSH_MAX_MINERS 100
#define RUSH_BUFFER_SIZE 6
#define RUSH_MQ_MAXMSG 7
#define RUSH_INITIAL_TARGET 0L

#define RUSH_BLOCK_RESULT 1
#define RUSH_BLOCK_FINISH 2

typedef struct
{
    int type;
    int round;
    pid_t winner_pid;
    long target;
    long solution;
    int is_valid;
    int votes_yes;
    int votes_total;
    int coins;
} RushBlock;

typedef struct
{
    int used;
    int active;
    pid_t pid;
    int coins;
    int joined_round;
    int vote_round;
    int vote;
} RushMinerInfo;

typedef struct
{
    sem_t sem_empty;
    sem_t sem_fill;
    sem_t sem_mutex;

    sem_t state_mutex;
    sem_t winner_mutex;

    int monitor_alive;
    pid_t checker_pid;
    pid_t monitor_pid;

    long current_target;
    long current_winner_target;
    long current_solution;
    pid_t current_winner;
    int current_round;
    int winner_round;
    int last_validation;
    int votes_yes;
    int votes_total;

    int buffer_in;
    int buffer_out;
    RushBlock buffer[RUSH_BUFFER_SIZE];

    int active_count;
    RushMinerInfo miners[RUSH_MAX_MINERS];
} RushShared;

#endif
