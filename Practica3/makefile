CC = gcc
CFLAGS = -Wall -Wextra -g -pthread
LDLIBS = -pthread -lrt

TARGETS = miner monitor
MINER_OBJS = miner.o pow.o
MONITOR_OBJS = monitor.o pow.o

.PHONY: all run clean

all: $(TARGETS)

run: all
	./monitor 10 5 & \
	MONITOR_PID=$$!; \
	sleep 1; \
	./miner 3 8 & \
	./miner 5 8 & \
	./miner 10 8 & \
	./miner 10 1; \
	wait $$MONITOR_PID

miner: $(MINER_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

monitor: $(MONITOR_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c miner_rush.h pow.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGETS) *.o *.log
	rm -f /dev/shm/soper_miner_rush_shm /dev/mqueue/soper_miner_rush_mq
