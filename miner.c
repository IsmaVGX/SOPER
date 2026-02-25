#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char *argv[])
{

    int i;
    pid_t pid;
    int n_hilos;
    int n_rounds;
    pid_t registrador;
    int target;


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
        printf("Registrador %d\n", i);
        exit(EXIT_SUCCESS);
    }
    else if (pid > 0)
    {
        printf("Soy el minero. Rondas: %d, Hilos: %d\n", n_rounds, n_hilos);

        int status;
        waitpid(pid, &status, 0);

        if (WIFEXITED(status))
        {
            printf("exit con status %d\n",WEXITSTATUS(status));
        }else{
            printf("exit unexpected\n");
        }
        
    }

    wait(NULL);
    printf("minero exit con status 0\n");
    exit(EXIT_SUCCESS);

}