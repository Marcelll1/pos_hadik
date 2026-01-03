CC=gcc
CFLAGS=-std=c11 -Wall -Wextra -Wpedantic -O2 -g -D_POSIX_C_SOURCE=200809L
LDLIBS=-lpthread

COMMON_SRC=common/protocol.c
SERVER_SRC=server/main.c server/game.c
CLIENT_SRC=client/main.c

SERVER_BIN=server_bin
CLIENT_BIN=client_bin

all: server client

server: $(SERVER_BIN)
client: $(CLIENT_BIN)

$(SERVER_BIN): $(COMMON_SRC) $(SERVER_SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(CLIENT_BIN): $(COMMON_SRC) $(CLIENT_SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

clean:
	rm -f $(SERVER_BIN) $(CLIENT_BIN)

