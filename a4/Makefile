PORT=59863
FLAGS = -DPORT=$(PORT) -g -Wall -std=gnu99

all: rcopy_client rcopy_server

rcopy_client: rcopy_client.o ftree.o hash_functions.o
	gcc ${FLAGS} -o $@ $^

rcopy_server: rcopy_server.o ftree.o hash_functions.o
	gcc ${FLAGS} -o $@ $^

%.o : %.c
	gcc ${FLAGS} -c $<

clean:
	rm -f *.o rcopy_client rcopy_server
