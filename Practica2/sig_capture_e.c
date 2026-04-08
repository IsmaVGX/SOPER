#define _POSIX_C_SOURCE 200809L
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

void handler(int sig) {
  printf("\n¡Recibida la señal número %d!\n", sig);
  fflush(stdout);
}

int main(void) {
  struct sigaction act;
  act.sa_handler = handler;
  sigemptyset(&(act.sa_mask));
  act.sa_flags = 0;

  // Intentamos registrar las señales de la 1 a la 31
  for (int i = 1; i <= 31; i++) {
    if (sigaction(i, &act, NULL) < 0) {
      // Si falla, imprimimos cuál ha sido y por qué
      fprintf(stderr, "Error: No se puede capturar la señal %d\n", i);
    }
  }

  printf("Proceso listo (PID = %d). Prueba con 'kill -<sig> %d'\n", getpid(), getpid());

  while (1) {
    pause(); // pause() es más eficiente que sleep() para esperar señales
  }
  return 0;
}