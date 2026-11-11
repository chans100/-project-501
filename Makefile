all: peer tracker

dir_setup:
	mkdir -p peer1
	mkdir -p peer2
	mkdir -p peer3

peer: peer.c dir_setup
	gcc peer.c -o peer1/peer
	gcc peer.c -o peer2/peer
	gcc peer.c -o peer3/peer

tracker: tracker.c
	gcc tracker.c -o tracker
	
clean:
	rm -f tracker
	rm -f peer1/peer
	rmdir peer1
	rm -f peer2/peer
	rmdir peer2
	rm -f peer3/peer
	rmdir peer3