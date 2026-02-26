CC = gcc
CFLAGS = -Wall -g
TARGET = Minero
OBJS = miner.o pow.o

all: $(TARGET)

run: $(TARGET)
	./$(TARGET) 10004 6 50
 
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS)

miner.o: miner.c pow.h
	$(CC) $(CFLAGS) -c $< -o $@

pow.o: pow.c pow.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) *.o