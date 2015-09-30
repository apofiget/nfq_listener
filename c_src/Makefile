
CC=gcc
CMD_DIR= $(dir$(lastword $(MAKEFILE_LIST)))../priv
CMD_PATH= $(CMD_DIR)/nfq_node

all: dirs compile

dirs:
	-@mkdir -p $(CMD_DIR)

compile:
	$(CC) -g -Wall -o $(CMD_PATH)
