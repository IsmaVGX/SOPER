# Variables del compilador
CC = gcc
# Banderas de compilación: -Wall muestra advertencias, -g permite usar gdb, -pthread para hilos
CFLAGS = -Wall -Wextra -g -pthread
# Banderas de enlazado (necesario para los hilos)
LDFLAGS = -pthread

# Nombre del ejecutable final
TARGET = miner

# Archivos fuente y objetos
SRCS = miner.c pow.c
OBJS = $(SRCS:.c=.o)

# Regla por defecto al escribir 'make'
all: $(TARGET)

# Regla para ejecutar
run: $(TARGET)
	./$(TARGET) 3 8 & ./$(TARGET) 5 8 & ./$(TARGET) 10 8 & ./$(TARGET) 10 1

# Regla para enlazar el ejecutable
$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

# Regla para compilar los archivos .c en archivos objeto .o
%.o: %.c pow.h
	$(CC) $(CFLAGS) -c $< -o $@

# Regla para limpiar el directorio de archivos compilados y logs (muy útil antes de entregar)
clean:
	rm -f $(OBJS) $(TARGET)
	rm -f pids.txt target.tgt votaciones.log *.log