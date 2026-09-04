.RECIPEPREFIX := >

CC ?= gcc
USE_AESD_CHAR_DEVICE ?= 1

CFLAGS ?= -g -Wall -Wextra -pthread
CFLAGS += -DUSE_AESD_CHAR_DEVICE=$(USE_AESD_CHAR_DEVICE)

LDFLAGS ?= -pthread -lrt

all: aesdsocket

default: aesdsocket

aesdsocket: aesdsocket.o
>$(CC) aesdsocket.o -o aesdsocket $(LDFLAGS)

aesdsocket.o: aesdsocket.c
>$(CC) $(CFLAGS) -c aesdsocket.c

clean:
>rm -f *.o aesdsocket *.elf *.map
