# Build the tracker and install the peer into peer1, peer2, and peer3.
#   make -f makefile
#   ./tracker
#   cd peer1 && ./peer

CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -O2
EXE =

ifeq ($(OS),Windows_NT)
	LIBS = -lws2_32
	EXE = .exe
else
	CFLAGS += -pthread
	LIBS = -pthread
endif

.PHONY: all clean

all: tracker$(EXE) peer$(EXE)
	mkdir -p peer1 peer2 peer3
	cp peer$(EXE) peer1/peer$(EXE)
	cp peer$(EXE) peer2/peer$(EXE)
	cp peer$(EXE) peer3/peer$(EXE)

tracker$(EXE): tracker.c netutil.h
	$(CC) $(CFLAGS) -o tracker$(EXE) tracker.c $(LIBS)

peer$(EXE): peer.c netutil.h
	$(CC) $(CFLAGS) -o peer$(EXE) peer.c $(LIBS)

clean:
	rm -f tracker$(EXE) peer$(EXE) peer1/peer$(EXE) peer2/peer$(EXE) peer3/peer$(EXE)
