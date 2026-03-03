#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include "pow.h"

/*Necesario para la variable global*/
#include <stdatomic.h>
#include <stdbool.h>

atomic_bool resuelto = false;
/*Variable por si dos hilos descubren la solucion exactamente a la vez*/
pthread_mutex_t mutex_solucion = PTHREAD_MUTEX_INITIALIZER;

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

int main(int argc, char *argv[])
{

    // int i;
    pid_t pid;
    int n_hilos;
    int n_rounds;
    int target_inicial;
    int contadorRondas = 0;
    int i = 0;
    datos_hilo *args;

    /*Comprobacion para que pete al tener un numero incorrecto de argumentos*/
    if (argc != 4)
    {
        printf("Error en los argumentos\n");
        return -1;
    }

    target_inicial = atoi(argv[1]);
    n_rounds = atoi(argv[2]);
    n_hilos = atoi(argv[3]);

    int pipe_ida[2];
    int pipe_vuelta[2]; /*Esto se usa desde el registrador hasta el minero y la de arrriba es desde el minero hasta el registrador*/

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

        snprintf(fichero_registrador, sizeof(fichero_registrador), "%d.log", parent_id);

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
            dprintf(archivo, "Winner: \t%d\n", parent_id);
            dprintf(archivo, "Target: \t%d\n", mensaje.target);

            if (mensaje.is_valid)
            {
                dprintf(archivo, "Solution: \t%08d (validated)\n", mensaje.solucion);
            }
            else
            {
                dprintf(archivo, "Solution: \t%08d (rejected)\n", mensaje.solucion);
            }

            dprintf(archivo, "Votes: \t\t%d/%d\n", mensaje.ronda, n_rounds);
            dprintf(archivo, "Wallets: \t%d:%d\n", parent_id, mensaje.ronda);

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

        int targetActual = target_inicial;


        for (contadorRondas = 1; contadorRondas <= n_rounds; contadorRondas++)
        {
            pthread_t threads[n_hilos];
            int escalon = POW_LIMIT / n_hilos;

            int solucionEncontrada = -1;
            atomic_store(&resuelto, false);

            for (i = 0; i < n_hilos; i++)
            {
                args = malloc(sizeof(datos_hilo));
                args->inicio = i * escalon;
                args->final = (i == n_hilos - 1) ? POW_LIMIT : (i + 1) * escalon;
                args->target = targetActual;
                args->solucion = &solucionEncontrada;

                if (pthread_create(&threads[i], NULL, funcionPow, args) != 0)
                {
                    perror("Error creando hilo");
                    exit(EXIT_FAILURE);
                }
            }

            for (i = 0; i < n_hilos; i++)
            {
                pthread_join(threads[i], NULL);
            }

            MensajePipe mensaje;
            mensaje.ronda = contadorRondas;
            mensaje.target = targetActual;

            if (solucionEncontrada != -1)
            {
                mensaje.solucion = solucionEncontrada;
                mensaje.is_valid = true;
                printf("Solution accepted: %08d --> %08d\n", targetActual, solucionEncontrada);
                targetActual = solucionEncontrada;
            }
            else
            {
                mensaje.solucion = -1;
                mensaje.is_valid = false;
                printf("Solution rejected: %08d\n", targetActual);
            }

            write(pipe_ida[1], &mensaje, sizeof(MensajePipe));
            int confirmacion;
            read(pipe_vuelta[0], &confirmacion, sizeof(int));
        }
        MensajePipe endMensaje;
        endMensaje.ronda = -1;
        write(pipe_ida[1], &endMensaje, sizeof(MensajePipe));

        close(pipe_ida[1]);
        close(pipe_vuelta[0]);

        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status))
        {
            printf("Logger exited with status %d\n", WEXITSTATUS(status));
        }
        else
        {
            printf("Logger exited unexpectedly\n");
        }

        printf("Miner exited with status 0\n");
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

    free(d); // Liberamos la memoria que reservamos en el main
    return NULL;
}