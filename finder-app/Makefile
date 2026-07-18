CC = $(CROSS_COMPILE)gcc
CFLAGS = -Wall -Werror

TARGET = writer

all: $(TARGET)

$(TARGET): writer.o
	$(CC) $(CFLAGS) -o $(TARGET) writer.o

writer.o: writer.c
	$(CC) $(CFLAGS) -c writer.c -o writer.o

clean:
	rm -f $(TARGET) writer.o
