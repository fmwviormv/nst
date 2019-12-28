.POSIX:
CFLAGS+= -std=c99 -pedantic -Wall -Wextra -Werror
PACKAGES+= libcrypto
#PACKAGES+= libbsd-overlay
CFLAGS+= -D_BSD_SOURCE -DUSE_PLEDGE -DUSE_UNVEIL
PACKAGES_CFLAGS!= pkg-config --cflags $(PACKAGES)
PACKAGES_LDFLAGS!= pkg-config --libs $(PACKAGES)
CFLAGS+= $(PACKAGES_CFLAGS)
LDFLAGS+= $(PACKAGES_LDFLAGS)

all: nstc nstd

clean:
	rm -f {nstc,nstd,addr2c,msg}{.o,.core,} addr.{t,c,o}

nstc: nstc.o msg.o addr.o
	$(CC) $(LDFLAGS) -o $@ nstc.o msg.o addr.o

nstd: nstd.o msg.o addr.o
	$(CC) $(LDFLAGS) -o $@ nstd.o msg.o addr.o

addr2c: addr2c.o
	$(CC) $(LDFLAGS) -o $@ addr2c.o

nstc.o nstd.o msg.o addr2c.o: msg.h

addr.c: addr2c Makefile
	echo '#define _POSIX_C_SOURCE 200809L'	>addr.t
	echo '#include <sys/socket.h>'		>>addr.t
	echo '#include <netdb.h>'		>>addr.t
	./addr2c client_ai	udp	127.0.0.1	8001 >>addr.t
	./addr2c server_ai	udp	127.0.0.1	8002 >>addr.t
	./addr2c listen_ai	tcp	127.0.0.1	8003 >>addr.t
	./addr2c connect_ai	tcp	127.0.0.1	8004 >>addr.t
	mv addr.t addr.c

.PHONY: all clean
