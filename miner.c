#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include <pthread.h>
#include "pow.h"

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

        target = pow_hash(target);

        pthread_t threads[n_hilos];

        for (int i = 0; i < n_hilos; i++)
        {
            pthread_create(&threads[i], NULL, funcion_hilo, NULL); //meTER AQUI una funcion, supongo
        }

        for (int i = 0; i < n_hilos; i++)
        {
            pthread_join(threads[i], NULL);
        }

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
    }

    wait(NULL);
    printf("minero exit con status 0\n");
    exit(EXIT_SUCCESS);
}