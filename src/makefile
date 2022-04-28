CC ?= gcc #Set c-compiler to gcc if env has not already set it to something else
CFLAGS ?= -Wall -Wextra -std=gnu11 -g #Set CFLAGS to give us additional warnings if these are not already set
OBJECT_FLAGS = -c

all: upush_server upush_client

# Compile objects, do not link
%.o : %.c
	$(CC) $(CFLAGS) $(OBJECT_FLAGS) $< -o $@

upush_server: upush_server.o send_packet.o nick_node_server.o network_utils.o utils.o
	$(CC) $(CFLAGS) upush_server.o send_packet.o nick_node_server.o network_utils.o utils.o -o upush_server

upush_client: upush_client.o send_node.o send_packet.o network_utils.o nick_node_client.o real_time.o utils.o
	$(CC) $(CFLAGS) upush_client.o send_node.o send_packet.o network_utils.o nick_node_client.o real_time.o utils.o -lrt -o upush_client

clean:
	rm -f upush_server upush_client *.o