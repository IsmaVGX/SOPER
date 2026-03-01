#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include <pthread.h>
#include <stdbool.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "pow.h"

typedef struct
{
    long target;     // El número objetivo que buscamos
    long start;      // Dónde empieza a buscar este hilo
    long end;        // Dónde termina de buscar este hilo
    long *solucion;  // Puntero compartido para escribir la solución
    int *encontrado; // Bandera compartida: 0 = buscando, 1 = encontrado
} ThreadArgs;

typedef struct
{
    int round;
    long target;
    long solution;
    bool is_valid;
} Message;

/* * NOTA: Más adelante tendrás que incluir "pow.h" para el minado,
 * pero para el apartado A (estructura de procesos) no es estrictamente necesario aún.
 */
void *miner_thread(void *arg);

int main(int argc, char *argv[])
{
    // 1. VALIDACIÓN DE ARGUMENTOS
    // El programa debe recibir exactamente 3 argumentos + el nombre del programa
    if (argc != 4)
    {
        fprintf(stderr, "Uso: %s <TARGET_INI> <ROUNDS> <N_THREADS>\n", argv[0]);
        return EXIT_FAILURE;
    }

    // 2. CONVERSIÓN DE ARGUMENTOS
    // Usamos 'long' para el target porque así lo define pow.h
    long target_ini = atol(argv[1]);
    int rounds = atoi(argv[2]);
    int n_threads = atoi(argv[3]);

    // crear las tuberias antes del fork
    int pipe_ida[2];
    int pipe_vuelta[2]; /*Esto se usa desde el registrador hasta el minero y la de arrriba es desde el minero hasta el registrador*/

    if (pipe(pipe_ida) == -1 || pipe(pipe_vuelta) == -1)
    {
        perror("pipe");
        exit(EXIT_FAILURE);
    }

    // 3. CREACIÓN DEL PROCESO HIJO (REGISTRADOR)
    pid_t pid = fork();

    if (pid < 0)
    {
        // ERROR: No se pudo crear el proceso
        perror("fork");
        exit(EXIT_FAILURE);
    }
    else if (pid == 0)
    {
        // --- CÓDIGO DEL PROCESO HIJO (REGISTRADOR) ---

        /* * Aquí irá la lógica del APARTADO C (leer tubería y escribir fichero).*/

        // 1. Cierre de tuberías no usadas
        close(pipe_ida[1]);    // No vamos a escribir hacia el registrador (somos nosotros), lee lo que el minero le dice
        close(pipe_vuelta[0]); // No vamos a leer la vuelta, escribe la solucion

        // Crear nombre fichero: <PPID>.log
        char filename[32];
        pid_t ppid = getppid();

        snprintf(filename, sizeof(filename), "%d.log", ppid);

        int archivo = open(filename, O_CREAT | O_TRUNC | O_RDWR, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
        if (archivo == -1)
        {
            perror("open");
            exit(EXIT_FAILURE);
        }
        Message mensaje;
        int confirmacion = 1;
        while (read(pipe_ida[0], &mensaje, sizeof(Message)) > 0)
        {

            if (mensaje.round == -1)
            {
                break;
            }

            dprintf(archivo, "Id:%d\n", mensaje.round);
            dprintf(archivo, "Winner:%d\n", ppid);
            dprintf(archivo, "Target:%08ld\n", mensaje.target); // %08ld para rellenar con ceros

            if (mensaje.is_valid)
            {
                dprintf(archivo, "Solution: %08ld (validated)\n", mensaje.solution);
            }
            else
            {
                dprintf(archivo, "Solution: %08ld (rejected)\n", mensaje.solution);
            }
            dprintf(archivo, "Votes:%d/%d\n", mensaje.round, mensaje.round);
            dprintf(archivo, "Wallets:%d:%d\n\n", ppid, mensaje.round);

            // IMPORTANTE: Enviar confirmación para despertar al padre
            write(pipe_vuelta[1], &confirmacion, sizeof(int));
        }

        // Limpieza hijo
        close(archivo);
        close(pipe_ida[0]);
        close(pipe_vuelta[1]);

            exit(EXIT_SUCCESS);
    }
    else
    {
        // --- CÓDIGO DEL PROCESO PADRE (MINERO) ---

        // Cerrar extremos no usados
        close(pipe_ida[0]);    // Solo escribimos ida
        close(pipe_vuelta[1]); // Solo leemos vuelta

        long current_target = target_ini; // El objetivo actual (cambia en cada ronda)
        long solucion_global = -1;
        int encontrado_global = 0;

        // Array de hilos y de argumentos
        pthread_t *threads = malloc(n_threads * sizeof(pthread_t));
        ThreadArgs *args = malloc(n_threads * sizeof(ThreadArgs));

        // BUCLE DE RONDAS
        for (int r = 0; r < rounds; r++)
        {
            encontrado_global = 0; // Reiniciar bandera para nueva ronda
            solucion_global = -1;

            // Calcular el tamaño del trozo para cada hilo
            long chunk_size = POW_LIMIT / n_threads;

            // 1. CREAR HILOS
            for (int i = 0; i < n_threads; i++)
            {
                args[i].target = current_target;
                args[i].solucion = &solucion_global;
                args[i].encontrado = &encontrado_global;

                // Definir rangos
                args[i].start = i * chunk_size;
                // El último hilo se lleva el resto (si la división no es exacta)
                if (i == n_threads - 1)
                {
                    args[i].end = POW_LIMIT;
                }
                else
                {
                    args[i].end = (i + 1) * chunk_size;
                }

                pthread_create(&threads[i], NULL, miner_thread, &args[i]);
            }

            // 2. ESPERAR HILOS
            for (int i = 0; i < n_threads; i++)
            {
                pthread_join(threads[i], NULL);
            }

            // 3. ACTUALIZAR PARA SIGUIENTE RONDA
            // C) COMUNICACIÓN CON EL REGISTRADOR (Apartado C)
            if (encontrado_global)
            {
                // 1. Preparar el mensaje con la solución
                Message msg;
                msg.round = r + 1;
                msg.target = current_target;
                msg.solution = solucion_global;
                msg.is_valid = true; // Por defecto es válida si el minero la encontró

                // 2. Enviar mensaje por la tubería
                write(pipe_ida[1], &msg, sizeof(Message));

                // 3. Imprimir feedback por pantalla [cite: 1100]
                if (msg.is_valid)
                {
                    printf("Solution accepted: %08ld --> %08ld\n", current_target, solucion_global);
                }
                else
                {
                    printf("Solution rejected: %08ld --> %08ld\n", current_target, solucion_global);
                }
                // 4. Esperar confirmación del Registrador (Sincronización)
                int ack;
                read(pipe_vuelta[0], &ack, sizeof(int));

                // 5. Preparar siguiente ronda
                current_target = solucion_global;
            }
            else
            {
                fprintf(stderr, "Error: Solución no encontrada en ronda %d\n", r + 1);
                break;
            }
        }

        // D) SEÑAL DE FINALIZACIÓN
        // Enviamos un mensaje especial para que el hijo salga del bucle read()
        Message fin;
        fin.round = -1;
        write(pipe_ida[1], &fin, sizeof(Message));

        // Limpieza de recursos del padre
        free(threads);
        free(args);
        close(pipe_ida[1]);
        close(pipe_vuelta[0]);

        // 4. ESPERA DEL PADRE AL HIJO
        // El minero debe esperar a que el registrador termine sus tareas.
        int status;
        if (waitpid(pid, &status, 0) == -1)
        {
            perror("waitpid");
            exit(EXIT_FAILURE);
        }

        // 5. COMPROBACIÓN DEL ESTADO DE SALIDA DEL HIJO
        // WIFEXITED(status) es verdadero si el hijo terminó con un exit() normal.
        if (WIFEXITED(status))
        {
            // WEXITSTATUS(status) extrae el número que el hijo puso en su exit().
            printf("Logger exited with status %d\n", WEXITSTATUS(status));
        }
        else
        {
            // Si el hijo murió por una señal o crash.
            printf("Logger exited unexpectedly\n");
        }

        // 6. SALIDA DEL PROPIO MINERO
        printf("Miner exited with status %d\n", EXIT_SUCCESS);
        exit(EXIT_SUCCESS);
    }
}

void *miner_thread(void *arg)
{
    ThreadArgs *args = (ThreadArgs *)arg;
    long i;

    // Bucle de búsqueda en el rango asignado
    for (i = args->start; i < args->end; i++)
    {
        // 1. Comprobar si otro hilo ya encontró la solución (Eficiencia)
        if (*(args->encontrado))
        {
            break; // Salir si ya hay ganador
        }

        // 2. Comprobar si este número es la solución
        if (pow_hash(i) == args->target)
        {
            *(args->solucion) = i;   // Guardar la solución
            *(args->encontrado) = 1; // Avisar a los demás
            break;
