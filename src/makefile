CC ?= gcc #Set c-compiler to gcc if env has not already set it to something else
CFLAGS ?= -Wall -Wextra -std=gnu11 #Set CFLAGS to give us additional warnings if these are not already set
OBJECT_FLAGS = -c

all: upush_server

# Compile objects, do not link
%.o : %.c
	$(CC) $(CFLAGS) $(OBJECT_FLAGS) $< -o $@

upush_server: upush_server.o send_packet.o
	$(CC) $(CFLAGS) upush_server.o send_packet.o -o upush_server

clean:
	rm -f upush_server *.o

