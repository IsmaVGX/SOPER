# Practica 3 - Ejercicio 7

## Compilacion

```sh
make
```

## Uso

El monitor debe arrancar primero:

```sh
./monitor <LAG_COMPROBADOR> <LAG_MONITOR>
```

Despues se pueden arrancar varios mineros:

```sh
./miner <N_SECS> <N_THREADS>
```

Ejemplo:

```sh
./monitor 10 5
./miner 3 8 & ./miner 5 8 & ./miner 10 8 & ./miner 10 1
```

Los mineros generan solo los ficheros de registrador `<pid>.log`.
Las estructuras temporales de la practica 2 (`pids.txt`, `target.tgt` y
`votaciones.log`) se han sustituido por memoria compartida POSIX.

## Recursos IPC

- Memoria compartida: `/soper_miner_rush_shm`.
- Cola de mensajes: `/soper_miner_rush_mq`.
- La cola tiene capacidad para 7 mensajes.
- El buffer circular Comprobador-Monitor tiene 6 bloques.
- Los semaforos del productor-consumidor estan alojados dentro de la memoria compartida.

Para limpiar binarios, logs y recursos IPC:

```sh
make clean
```

## Respuestas apartado 7d

Si el Comprobador es mucho mas lento que los mineros, si es necesario algun mecanismo
de sincronizacion o almacenamiento intermedio. En esta solucion la cola de mensajes
absorbe hasta 7 bloques y bloquea al minero si se llena, de forma que no se pierden
resultados.

Si el Comprobador es mas rapido que los mineros, el productor-consumidor entre
Comprobador y Monitor sigue siendo necesario para desacoplar ritmos y evitar que el
Monitor lea posiciones vacias del buffer.

Usar otra cola entre Comprobador y Monitor simplificaria parte del codigo porque la
cola ya implementa bloqueo y almacenamiento ordenado. Aun asi, el enunciado pide
usar memoria compartida con buffer circular y tres semaforos sin nombre, por lo que
esa comunicacion se implementa explicitamente.
