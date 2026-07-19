all: clean aesdsocket
default: aesdsocket

aesdsocket:
	$(CC) -g -Wall -c -o aesdsocket.o aesdsocket.c
	$(CC) -g -Wall aesdsocket.o -o aesdsocket

clean:
	rm -f *.o aesdsocket *.elf *.map
