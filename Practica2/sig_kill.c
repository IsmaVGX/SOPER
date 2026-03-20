#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>


int main(int argc, char *argv[]) {
  int sig;
  pid_t pid;

  if (argc != 3) {
    fprintf(stderr, "Usage: %s -<signal> <pid>\n", argv[0]);
    exit(EXIT_FAILURE);
  }

  sig = atoi(argv[1] + 1);
  pid = (pid_t)atoi(argv[2]);

  /* Enviamos la señal 'sig' al proceso 'pid' */
  if (kill(pid, sig) == -1) {
    perror("Error al enviar la señal"); // Imprime el motivo del fallo
    exit(EXIT_FAILURE);
  }

  printf("Señal %d enviada con éxito al proceso %d\n", sig, pid);

  exit(EXIT_SUCCESS);
}
