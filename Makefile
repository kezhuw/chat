CC = gcc
CFLAGS = -std=c99 -Wall -Wextra -Werror -Wno-unused-function
all: client server
client: client.c pipe.c util.c
	$(CC) $(CFLAGS) client.c pipe.c util.c -o client
server: server.c pipe.c util.c
	$(CC) $(CFLAGS) server.c pipe.c util.c -lpthread -o server
clean:
	rm -rf client server *.o *.out
