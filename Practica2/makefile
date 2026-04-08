CC = gcc
CFLAGS = -Wall -Wextra -g -pthread
LDFLAGS = -pthread
TARGET = miner
SRCS = miner.c pow.c
OBJS = $(SRCS:.c=.o)
all: $(TARGET)

run: $(TARGET)
	./$(TARGET) 3 8 & ./$(TARGET) 5 8 & ./$(TARGET) 10 8 & ./$(TARGET) 10 1

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

%.o: %.c pow.h
	$(CC) $(CFLAGS) -c $< -o $@
clean:
	rm -f $(OBJS) $(TARGET)
	rm -f pids.txt target.tgt votaciones.log *.log
