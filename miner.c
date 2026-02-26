#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include <pthread.h>
#include "pow.h"

/*Necesario para la variable global*/
#include <stdatomic.h>
#include <stdbool.h>

atomic_bool resuelto = false;

typedef struct
{
    int inicio;
    int final;
    int target;
} datos_hilo;

void *funcionPow(void *arg);

int main(int argc, char *argv[])
{

    // int i;
    pid_t pid;
    int n_hilos;
    int n_rounds;
    pid_t registrador;
    int target;

    /*Comprobacion para que pete al tener un numero incorrecto de argumentos*/
    if (argc != 4)
    {
        printf("Error en los argumentos\n");
        return -1;
    }

    target = atoi(argv[1]);
    n_rounds = atoi(argv[2]);
    n_hilos = atoi(argv[3]);

    pid = fork();

    if (pid < 0)
    {
        perror("fork");
        exit(EXIT_FAILURE);
    }
    else if (pid == 0)
    {
        printf("Registrador %d\n", getpid());
        exit(EXIT_SUCCESS);
    }
    else if (pid > 0)
    {
        printf("Soy el minero. Rondas: %d, Hilos: %d\n", n_rounds, n_hilos);

        pthread_t threads[n_hilos];
        int escalon = POW_LIMIT / n_hilos;

        for (int i = 0; i < n_hilos; i++)
        {
            datos_hilo *args = malloc(sizeof(datos_hilo));
            args->inicio = i * escalon;
            args->final = (i + 1) * escalon;
            args->target = target;

            if (pthread_create(&threads[i], NULL, funcionPow, args) != 0)
            {
                perror("Error creando hilo");
            }
        }

        for (int i = 0; i < n_hilos; i++)
        {
            pthread_join(threads[i], NULL);
        }

        /*Sale del minero*/
        int status;
        waitpid(pid, &status, 0);

        if (WIFEXITED(status))
        {
            printf("exit con status %d\n", WEXITSTATUS(status));
        }
        else
        {
            printf("exit unexpected\n");
        }

        wait(NULL);
        printf("minero exit con status 0\n");
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
            printf("\n[Hilo] ¡Encontrado! Valor: %d\n", i);
        }
    }

    free(d); // Liberamos la memoria que reservamos en el main
    return NULL;
}