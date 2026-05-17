default: animate_server animate_client

animate_server: animate_server.c
	gcc -Wall -Wextra -std=c11 $^ -I. -lanimate -lpthread -o $@

animate_client: animate_client.c
	gcc -Wall -Wextra -std=c11 $^ -o $@

clean:
	rm -f animate_server animate_client
	rm -f FIFO_*
	rm -f *.gif
	rm -f *.mp4
	rm -f *.dat