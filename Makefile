CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -D_GNU_SOURCE -I.
LDFLAGS = -lpthread

default: animate_server animate_client

# 同时编译 animate_server.c 和 animate.c
animate_server: animate_server.c animate.c
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

animate_client: animate_client.c
	$(CC) $(CFLAGS) $^ -o $@

clean:
	rm -f animate_server animate_client FIFO_* *.mp4 *.dat *.log