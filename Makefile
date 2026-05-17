default: animate_server animate_client

libanimate:
	@if [ ! -d $@ ]; then \
		echo "ERROR: libanimate not found" > /dev/stderr; \
		echo "Download and unzip libanimate.zip from P2 resources" > /dev/stderr; \
		false; \
	fi

animate_server: animate_server.c | libanimate
	gcc -Wall -Wextra -std=c11 $^ \
	-I libanimate/include \
	-Llibanimate/lib \
	-lanimate \
	-lpthread \
	-o $@

animate_client: animate_client.c
	gcc -Wall -Wextra -std=c11 $^ -o $@

clean:
	rm -f animate_server animate_client
	rm -f FIFO_*
	rm -f *.gif
	rm -f *.mp4
	rm -f *.dat