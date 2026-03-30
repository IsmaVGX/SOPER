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

atomic_bool resuelto = false;
pthread_mutex_t mutex_solucion = PTHREAD_MUTEX_INITIALIZER;

// Para saber cuándo acaba el tiempo
volatile sig_atomic_t tiempo_agotado = 0;

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
} MensajePipe;

void *funcionPow(void *arg);

// Cuando salta la alarma de los N_SEC cambiamos la variable para salir del bucle
void manejador_alarma(int sig)
{
    tiempo_agotado = 1;
}

// He sacado esto a una funcion para no tener un main infinito que sino gemini me decia que era muy gitano tenerlo en el main
void agregar_minero(pid_t mi_pid, sem_t *mutex)
{
    sem_wait(mutex);

    FILE *f = fopen(PIDS_FILE, "a+");
    if (f != NULL)
    {
        fprintf(f, "%d\n", mi_pid);
        rewind(f);

        printf("Miner %d added to system\n", mi_pid);
        printf("Current miners: ");
        int pid_leido;
        while (fscanf(f, "%d", &pid_leido) == 1)
        {
            printf("%d ", pid_leido);
        }
        printf("\n");
        fclose(f);
    }

    sem_post(mutex);
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
        // Si es el ultimo, se zumba el fichero y el semaforo.
        remove(PIDS_FILE);
        sem_unlink(SEM_MUTEX);
        printf("Miner %d exited system\n", mi_pid);
        printf("Current miners: [None, system closed]\n");
    }
    else
    {
        // Si quedan mas se reescribe el fichero actualizado
        f = fopen(PIDS_FILE, "w");
        printf("Miner %d exited system\n", mi_pid);
        printf("Current miners: ");
        for (int i = 0; i < num_pids; i++)
        {
            fprintf(f, "%d\n", pids_activos[i]);
            printf("%d ", pids_activos[i]);
        }
        printf("\n");
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
        printf("Uso: %s <N_SECS> <N_THREADS>\n", argv);
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
        close(pipe_vuelta);

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

        while (read(pipe_ida, &mensaje, sizeof(MensajePipe)) > 0)
        {
            if (mensaje.ronda == -1)
                break;

            dprintf(archivo, "Id: \t\t%d\n", mensaje.ronda);
            dprintf(archivo, "Winner: \t%jd\n", (intmax_t)parent_id);
            dprintf(archivo, "Target: \t%d\n", mensaje.target);

            if (mensaje.is_valid)
            {
                dprintf(archivo, "Solution: \t%08d (validated)\n", mensaje.solucion);
            }
            else
            {
                dprintf(archivo, "Solution: \t%08d (rejected)\n", mensaje.solucion);
            }

            // Solo quité n_rounds porque ya no existe como parámetro.
            dprintf(archivo, "Votes: \t\t%d\n", mensaje.ronda);
            dprintf(archivo, "Wallets: \t%jd:%d\n", (intmax_t)parent_id, mensaje.ronda);
            dprintf(archivo, "\n");

            write(pipe_vuelta[1], &confirmacion, sizeof(int));
        }

        close(archivo);
        close(pipe_ida);
        close(pipe_vuelta[1]);

        exit(EXIT_SUCCESS);
    }
    else if (pid > 0)
    {
        close(pipe_ida);
        close(pipe_vuelta[1]);

        pid_t mi_pid = getpid();

        // Alarma para que llame al manejador
        struct sigaction act;
        act.sa_handler = manejador_alarma;
        sigemptyset(&(act.sa_mask));
        act.sa_flags = 0;
        if (sigaction(SIGALRM, &act, NULL) < 0)
        {
            perror("Error configurando sigaction");
            exit(EXIT_FAILURE);
        }

        // Semaforo para proteger el archivo txt de pid
        sem_t *mutex = sem_open(SEM_MUTEX, O_CREAT, 0644, 1);
        if (mutex == SEM_FAILED)
        {
            perror("Error inicializando el semáforo");
            exit(EXIT_FAILURE);
        }

        agregar_minero(mi_pid, mutex);
        alarm(n_secs);

        while (!tiempo_agotado)
        {
            pause();
        }

        eliminar_minero(mi_pid, mutex);
        sem_close(mutex);

        MensajePipe endMensaje;
        endMensaje.ronda = -1;
        write(pipe_ida[1], &endMensaje, sizeof(MensajePipe));

        close(pipe_ida[1]);
        close(pipe_vuelta);

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