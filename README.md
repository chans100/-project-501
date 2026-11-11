# Compiling the Program
To create the program for execution run the following command in the directory:

```$ make -f makefile```

# Executing the Program

## Server
On the server instance run the following:

```$ ./tracker```

## Clients
You will need to run three separate clients. Each client has its own folder (`peer1`, `peer2`, `peer3`) where you will run the following to set them upl

```$ ./peer```

Alternatively run the following from the root directory, running only one command per client.

1. ```$ ./peer1/peer```
2. ```$ ./peer2/peer```
3. ```$ ./peer3/peer```

# Project Structure
## `./torrents`
All tracker files are created and stored in torrents. This is the shared directory of the root server. This shared directory and port number are read from the
configuration file `sconfig.cfg`.

## `./Reference `
Includes reference files for development that are not directly used by the program. 

## `./peer1` `./peer2` `./peer3`
These folders isolate each of the client instances to simulate multiple peers on a single machine.

### `./peer*/torrents`
The shared folder for each peer.
