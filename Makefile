CFLAGS=-O2 -g -Wall -pthread -std=gnu99
LDFLAGS=-pthread -lm

randomio: randomio.o

clean:
	rm -f *.o randomio
