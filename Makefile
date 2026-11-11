all: peer tracker

peer: peer.c
	gcc peer.c -o peer1/peer
	gcc peer.c -o peer2/peer
	gcc peer.c -o peer3/peer

tracker: tracker.c
	gcc tracker.c -o tracker
	
clean:
	rm -f tracker
	rm -f peer1/peer
	rm -f peer2/peer
	rm -f peer3/peer