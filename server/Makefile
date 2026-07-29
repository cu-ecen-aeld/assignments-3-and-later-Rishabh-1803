CC ?= gcc
CFLAGS ?= -g -Wall -pthread
LDFLAGS ?= -pthread -lrt

all: aesdsocket

default: aesdsocket

aesdsocket: aesdsocket.o
	$(CC) aesdsocket.o -o aesdsocket $(LDFLAGS)

aesdsocket.o: aesdsocket.c
	$(CC) $(CFLAGS) -c aesdsocket.c

clean:
	rm -f *.o aesdsocket *.elf *.map
