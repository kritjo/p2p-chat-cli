#Set c-compiler to gcc if env has not already set it to something else
CC ?= gcc
#Set CFLAGS to give us additional warnings if these are not already set
CFLAGS ?= -Wall -Wextra -std=gnu11 -g
OBJECT_FLAGS = -c

all: upush_server upush_client

# Compile objects, do not link
%.o : %.c
	$(CC) $(CFLAGS) $(OBJECT_FLAGS) $< -o $@

upush_server: upush_server.o send_packet.o network_utils.o common.o linked_list.o
	$(CC) $(CFLAGS) upush_server.o send_packet.o network_utils.o common.o linked_list.o -o upush_server

upush_client: upush_client.o send_packet.o network_utils.o real_time.o common.o linked_list.o
	$(CC) $(CFLAGS) upush_client.o send_packet.o network_utils.o real_time.o common.o linked_list.o -lrt -o upush_client

clean:
	rm -f upush_server upush_client *.o