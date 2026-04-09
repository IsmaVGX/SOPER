#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include "pow.h"
#include <stdint.h>

#include <signal.h>
#include <semaphore.h>
#include <sys/stat.h>

#include <stdatomic.h>
#include <stdbool.h>

#define PIDS_FILE "pids.txt"
#define SEM_MUTEX "/sem_miner_mutex"
#define SEM_WINNER "/sem_miner_winner"
#define FICHERO "target.tgt"

atomic_bool resuelto = false;
pthread_mutex_t mutex_solucion = PTHREAD_MUTEX_INITIALIZER;

volatile sig_atomic_t tiempo_agotado = 0;
volatile sig_atomic_t iniciada = 0;
volatile sig_atomic_t alguien_gano = 0;

typedef struct
{
    int inicio;
    int final;
    int target;
    int *solucion;
} datos_hilo;

typedef struct
{
    int ronda;
    int target;
    int solucion;
    bool is_valid;
    int votos_favor;
    int votos_total;
    int monedas_ganadas;
    int pid_ganador;
} MensajePipe;

void *funcionPow(void *arg);

void manejador_alarma(int sig)
{

    tiempo_agotado = 1;
    (void)sig;
}

void manejador_ronda(int sig)
{
    iniciada = 1;
    (void)sig;
}
void manejador_victoria(int sig)
{
    alguien_gano = 1;
    atomic_store(&resuelto, true);
    (void)sig;
}

int agregar_minero(pid_t mi_pid, sem_t *mutex)
{
    int primer = 1;

    sem_wait(mutex);

    FILE *f = fopen(PIDS_FILE, "a+");
    if (f != NULL)
    {
        rewind(f);

        int pid_leido;
        int num_pids = 0;
        while (fscanf(f, "%d", &pid_leido) == 1)
        {
            num_pids++;
        }
        if (num_pids > 0)
        {
            primer = 0;
        }
        fprintf(f, "%d\n", mi_pid);
        rewind(f);
        while (fscanf(f, "%d", &pid_leido) == 1)
            fclose(f);
    }
    sem_post(mutex);
    return primer;
}

void eliminar_minero(pid_t mi_pid, sem_t *mutex)
{
    sem_wait(mutex);

    FILE *f = fopen(PIDS_FILE, "r");
    if (f == NULL)
    {
        sem_post(mutex);
        return;
    }

    int pids_activos[100];
    int num_pids = 0, pid_leido;

    while (fscanf(f, "%d", &pid_leido) == 1)
    {
        if (pid_leido != mi_pid)
        {
            pids_activos[num_pids++] = pid_leido;
        }
    }
    fclose(f);

    if (num_pids == 0)
    {
        remove(PIDS_FILE);
        sem_unlink(SEM_MUTEX);
    }
    else
    {
        f = fopen(PIDS_FILE, "w");
        for (int i = 0; i < num_pids; i++)
        {
            fprintf(f, "%d\n", pids_activos[i]);
        }
        fclose(f);
    }
    sem_post(mutex);
}

int main(int argc, char *argv[])
{
    pid_t pid;
    int n_hilos;
    int n_secs;

    if (argc != 3)
    {
        printf("Uso: %s <N_SECS> <N_THREADS>\n", argv[0]);
        return -1;
    }
    n_secs = atoi(argv[1]);
    n_hilos = atoi(argv[2]);

    int pipe_ida[2];
    int pipe_vuelta[2];
    if (pipe(pipe_ida) == -1 || pipe(pipe_vuelta) == -1)
    {
        perror("pipe");
        exit(EXIT_FAILURE);
    }

    pid = fork();

    if (pid < 0)
    {
        perror("fork");
        exit(EXIT_FAILURE);
    }
    else if (pid == 0)
    {
        close(pipe_ida[1]);
        close(pipe_vuelta[0]);

        pid_t parent_id = getppid();
        char fichero_registrador[64];

        snprintf(fichero_registrador, sizeof(fichero_registrador), "%jd.log", (intmax_t)parent_id);

        int archivo = open(fichero_registrador, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (archivo == -1)
        {
            perror("Error abriendo el archivo con open");
            exit(EXIT_FAILURE);
        }

        MensajePipe mensaje;
        int confirmacion = 1;

        while (read(pipe_ida[0], &mensaje, sizeof(MensajePipe)) > 0)
        {
            if (mensaje.ronda == -1)
                break;

            dprintf(archivo, "Id: \t\t%d\n", mensaje.ronda);
            dprintf(archivo, "Winner: \t%d\n", mensaje.pid_ganador);
            dprintf(archivo, "Target: \t%d\n", mensaje.target);

            if (mensaje.is_valid)
            {
                dprintf(archivo, "Solution: \t%08d (validated)\n", mensaje.solucion);
            }
            else
            {
                dprintf(archivo, "Solution: \t%08d (rejected)\n", mensaje.solucion);
            }
            dprintf(archivo, "Votes: \t\t%d/%d\n", mensaje.votos_favor, mensaje.votos_total);
            dprintf(archivo, "Wallets: \t%jd:%d\n", (intmax_t)parent_id, mensaje.monedas_ganadas);
            dprintf(archivo, "\n");
            write(pipe_vuelta[1], &confirmacion, sizeof(int));
        }

        close(archivo);
        close(pipe_ida[0]);
        close(pipe_vuelta[1]);

        exit(EXIT_SUCCESS);
    }
    else if (pid > 0)
    {
        close(pipe_ida[0]);
        close(pipe_vuelta[1]);
        int monedas = 0;
        pid_t mi_pid = getpid();

        struct sigaction act;
        act.sa_handler = manejador_alarma;
        sigfillset(&(act.sa_mask));
        act.sa_flags = 0;
        if (sigaction(SIGALRM, &act, NULL) < 0)
        {
            perror("Error configurando sigaction");
            exit(EXIT_FAILURE);
        }
        struct sigaction act2;
        act2.sa_handler = manejador_ronda;
        sigfillset(&(act2.sa_mask));
        act2.sa_flags = 0;
        if (sigaction(SIGUSR1, &act2, NULL) < 0)
        {
            exit(EXIT_FAILURE);
        }
        struct sigaction act3;
        act3.sa_handler = manejador_victoria;
        sigfillset(&(act3.sa_mask));
        act3.sa_flags = 0;
        if (sigaction(SIGUSR2, &act3, NULL))
        {
            exit(EXIT_FAILURE);
        }

        sigset_t mask, oldmask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGUSR1);
        sigaddset(&mask, SIGUSR2);
        sigaddset(&mask, SIGALRM);

        sem_t *mutex = sem_open(SEM_MUTEX, O_CREAT, 0644, 1);
        if (mutex == SEM_FAILED)
        {
            perror("Error inicializando el semáforo");
            exit(EXIT_FAILURE);
        }
        sem_t *sem_winner = sem_open(SEM_WINNER, O_CREAT, 0644, 1);
        if (sem_winner == SEM_FAILED)
        {
            perror("Error inicializando el semáforo de ganador");
            exit(EXIT_FAILURE);
        }
        alarm(n_secs);

        int primero;
        primero = agregar_minero(mi_pid, mutex);
        if (primero == 1)
        {
            sem_wait(mutex);

            FILE *file = fopen(FICHERO, "w");
            if (file != NULL)
            {
                fprintf(file, "-1 0\n");
                fclose(file);
            }

            sem_post(mutex);

            int njugadores = 0;

            while (njugadores < 2 && tiempo_agotado == 0)
            {
                sleep(1);
                sem_wait(mutex);
                FILE *file = fopen(PIDS_FILE, "r");
                if (file != NULL)
                {
                    njugadores = 0;
                    int leido;

                    while (fscanf(file, "%d", &leido) == 1)
                    {
                        njugadores++;
                    }
                    fclose(file);
                }
                sem_post(mutex);
            }
            if (tiempo_agotado == 0)
            {
                sem_wait(mutex);
                file = fopen(PIDS_FILE, "r");
                if (file != NULL)
                {
                    int pid_leido;
                    while (fscanf(file, "%d", &pid_leido) == 1)
                    {

                        kill(pid_leido, SIGUSR1);
                    }
                    fclose(file);
                }
                sem_post(mutex);
            }
            else
            {
                sigprocmask(SIG_BLOCK, &mask, &oldmask);
                while (iniciada == 0 && tiempo_agotado == 0)
                {
                    sigsuspend(&oldmask);
                }
                sigprocmask(SIG_SETMASK, &oldmask, NULL);
            }
        }
        else
        {
            sigprocmask(SIG_BLOCK, &mask, &oldmask);
            while (iniciada == 0 && tiempo_agotado == 0)
            {
                sigsuspend(&oldmask);
            }
            sigprocmask(SIG_SETMASK, &oldmask, NULL);
        }
        int r_actual = 0;

        while (tiempo_agotado == 0)
        {
            int num_mineros_activos = 0;
            sem_wait(mutex);
            FILE *f_check = fopen(PIDS_FILE, "r");
            if (f_check)
            {
                int pid;
                while (fscanf(f_check, "%d", &pid) == 1)
                {
                    num_mineros_activos++;
                }
                fclose(f_check);
            }
            sem_post(mutex);

            if (num_mineros_activos < 2)
            {
                sleep(1);
                continue;
            }

            r_actual++;
            alguien_gano = 0;
            iniciada = 0;
            atomic_store(&resuelto, false);
            int solucion = -1;
            int t_actual = 0;

            sem_wait(mutex);
            FILE *file = fopen(FICHERO, "r");
            if (file)
            {
                int pid_2;
                fscanf(file, "%d %d", &pid_2, &t_actual);
                fclose(file);
            }
            sem_post(mutex);
            pthread_t hilos[n_hilos];
            int rango = POW_LIMIT / n_hilos;
            int i;
            for (i = 0; i < n_hilos; i++)
            {
                datos_hilo *datos = malloc(sizeof(datos_hilo));
                datos->inicio = i * rango;
                if (i == n_hilos - 1)
                {
                    datos->final = POW_LIMIT;
                }
                else
                {
                    datos->final = (i + 1) * rango;
                }
                datos->target = t_actual;
                datos->solucion = &solucion;

                if (pthread_create(&hilos[i], NULL, funcionPow, datos) != 0)
                {
                    perror("Error creando hilo");
                    free(datos);
                }
            }
            for (int i = 0; i < n_hilos; i++)
            {
                pthread_join(hilos[i], NULL);
            }
            if (solucion != -1)
            {
                if (sem_trywait(sem_winner) == 0)
                {
                    alguien_gano = 0;

                    sem_wait(mutex);
                    FILE *f_clean = fopen("votaciones.log", "w");
                    if (f_clean)
                        fclose(f_clean);
                    sem_post(mutex);

                    sem_wait(mutex);
                    FILE *file_tgt = fopen(FICHERO, "w");
                    if (file_tgt != NULL)
                    {
                        fprintf(file_tgt, "%d %d\n", mi_pid, solucion);
                        fclose(file_tgt);
                    }
                    sem_post(mutex);
                    sem_wait(mutex);
                    FILE *f_pids = fopen(PIDS_FILE, "r");
                    if (f_pids != NULL)
                    {
                        int pid_dest;
                        while (fscanf(f_pids, "%d", &pid_dest) == 1)
                        {
                            if (pid_dest != mi_pid)
                                kill(pid_dest, SIGUSR2);
                        }
                        fclose(f_pids);
                    }
                    sem_post(mutex);

                    int num_mineros = 0, v = 0, n = 0, intentos = 0;
                    sem_wait(mutex);
                    FILE *f_pids_count = fopen(PIDS_FILE, "r");
                    if (f_pids_count)
                    {
                        int tmp;
                        while (fscanf(f_pids_count, "%d", &tmp) == 1)
                            num_mineros++;
                        fclose(f_pids_count);
                    }
                    sem_post(mutex);

                    int esperados = num_mineros - 1;
                    char cadena_votos[256] = "";
                    while (intentos < 3 && (v + n) < esperados && tiempo_agotado == 0)
                    {
                        v = 0;
                        n = 0;
                        int pos = 0;
                        sem_wait(mutex);
                        FILE *f_votos = fopen("votaciones.log", "r");
                        if (f_votos)
                        {
                            char c;
                            while (fscanf(f_votos, " %c", &c) == 1)
                            {
                                if (c == 'V')
                                {
                                    v++;
                                    cadena_votos[pos++] = ' ';
                                    cadena_votos[pos++] = 'Y';
                                }
                                else if (c == 'N')
                                {
                                    n++;
                                    cadena_votos[pos++] = ' ';
                                    cadena_votos[pos++] = 'N';
                                }
                            }
                            fclose(f_votos);
                        }
                        sem_post(mutex);

                        cadena_votos[pos] = '\0';

                        if ((v + n) < esperados)
                        {
                            sleep(1);
                            intentos++;
                        }
                    }

                    if (v >= n && (v + n) > 0)
                    {
                        printf("Winner %d => [%s ] => Accepted\n", mi_pid, cadena_votos);
                        monedas++;
                        sem_wait(mutex);
                        FILE *f_new_tgt = fopen(FICHERO, "w");
                        if (f_new_tgt)
                        {
                            fprintf(f_new_tgt, "%d %d\n", mi_pid, solucion);
                            fclose(f_new_tgt);
                        }
                        sem_post(mutex);
                    }
                    else
                    {
                        printf("Winner %d => [%s] => Rejected\n", mi_pid, cadena_votos);
                    }
                    MensajePipe msg;
                    msg.ronda = r_actual;
                    msg.target = t_actual;
                    msg.solucion = solucion;
                    msg.is_valid = (v >= n && (v + n) > 0);
                    msg.votos_favor = v;
                    msg.votos_total = v + n;
                    msg.monedas_ganadas = monedas;
                    msg.pid_ganador = mi_pid;
                    write(pipe_ida[1], &msg, sizeof(MensajePipe));

                    int confirmacion;
                    read(pipe_vuelta[0], &confirmacion, sizeof(int));
                    sem_wait(mutex);
                    FILE *f_pids_arranque = fopen(PIDS_FILE, "r");
                    if (f_pids_arranque)
                    {
                        int pid_dest;
                        while (fscanf(f_pids_arranque, "%d", &pid_dest) == 1)
                        {
                            if (pid_dest != mi_pid)
                                kill(pid_dest, SIGUSR1);
                        }
                        fclose(f_pids_arranque);
                    }
                    sem_post(mutex);

                    sem_post(sem_winner);
                }
                else
                {
                    alguien_gano = 1;
                }
            }
            if (alguien_gano == 1)
            {
                int solucion_winner = 0;
                int pid_del_ganador = 0;
                sem_wait(mutex);
                FILE *file_read = fopen(FICHERO, "r");
                if (file_read != NULL)
                {
                    fscanf(file_read, "%d %d", &pid_del_ganador, &solucion_winner);
                    fclose(file_read);
                }

                bool es_valida = false;
                FILE *file2 = fopen("votaciones.log", "a");
                if (file2 != NULL)
                {
                    if (pow_hash(solucion_winner) == t_actual)
                    {
                        fprintf(file2, " V ");
                        es_valida = true;
                    }
                    else
                    {
                        fprintf(file2, " N ");
                        es_valida = false;
                    }
                    fclose(file2);
                }
                sem_post(mutex);

                MensajePipe msg;
                msg.ronda = r_actual;
                msg.target = t_actual;
                msg.solucion = solucion_winner;
                msg.is_valid = es_valida;
                if (es_valida)
                {
                    msg.votos_favor = 1;
                }
                else
                {
                    msg.votos_favor = 0;
                }
                msg.votos_total = num_mineros_activos - 1;
                msg.monedas_ganadas = monedas;
                msg.pid_ganador = pid_del_ganador;
                write(pipe_ida[1], &msg, sizeof(MensajePipe));

                int confirmacion;
                read(pipe_vuelta[0], &confirmacion, sizeof(int));
                iniciada = 0;
                sigprocmask(SIG_BLOCK, &mask, &oldmask);
                while (iniciada == 0 && tiempo_agotado == 0)
                {
                    sigsuspend(&oldmask);
                }
                sigprocmask(SIG_SETMASK, &oldmask, NULL);
            }

            if (tiempo_agotado == 1)
            {
                break;
            }
        }
        eliminar_minero(mi_pid, mutex);
        sem_close(sem_winner);
        sem_unlink(SEM_WINNER);
        MensajePipe endMensaje;
        endMensaje.ronda = -1;
        write(pipe_ida[1], &endMensaje, sizeof(MensajePipe));

        close(pipe_ida[1]);
        close(pipe_vuelta[0]);

        int status;
        waitpid(pid, &status, 0);

        exit(EXIT_SUCCESS);
    }
}

void *funcionPow(void *arg)
{
    datos_hilo *d = arg;

    for (int i = d->inicio; i < d->final && !atomic_load(&resuelto); i++)
    {
        if (pow_hash(i) == d->target)
        {
            atomic_store(&resuelto, true);
            *(d->solucion) = i;
            pthread_mutex_unlock(&mutex_solucion);
        }
    }

    free(d);
    return NULL;
}
