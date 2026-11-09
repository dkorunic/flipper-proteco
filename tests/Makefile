FW ?= ../../unleashed-firmware
RAW ?=

test:
	$(CC) -std=gnu11 -Wall -Wextra -Werror -I$(FW)/lib -o /tmp/proteco_test_codec test_codec.c ../proteco_codec.c
	/tmp/proteco_test_codec $(RAW)
