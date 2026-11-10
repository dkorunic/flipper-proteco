FW ?= ../../unleashed-firmware
RAW ?= $(wildcard ../dumps/*.sub)
OUT := /tmp/proteco_test

# The firmware's own Proteco protocol (from the patch) is built as is from $(FW). It is copied
# next to a one-line environment.h, the only header it reaches that drags in the whole of furi.
SUBGHZ := $(FW)/lib/subghz
FW_SRC := types.h protocols/base.h protocols/common.h protocols/common.c protocols/proteco.h \
          protocols/proteco.c blocks/const.h blocks/decoder.h blocks/decoder.c blocks/encoder.h \
          blocks/generic.h blocks/math.h blocks/custom_btn.h blocks/custom_btn_i.h blocks/custom_btn.c

test:
	rm -rf $(OUT) && mkdir -p $(OUT)/subghz/protocols $(OUT)/subghz/blocks
	for f in $(FW_SRC); do cp $(SUBGHZ)/$$f $(OUT)/subghz/$$f || exit 1; done
	echo 'typedef struct SubGhzProtocol SubGhzProtocol;' > $(OUT)/subghz/environment.h
	$(CC) -std=gnu11 -Wall -Wextra -Werror -Ifw_stub -I$(FW) -I$(FW)/lib -I$(OUT)/subghz \
		-o $(OUT)/test_codec test_codec.c ../proteco_codec.c \
		$(OUT)/subghz/protocols/proteco.c $(OUT)/subghz/protocols/common.c \
		$(OUT)/subghz/blocks/decoder.c $(OUT)/subghz/blocks/custom_btn.c
	$(OUT)/test_codec $(RAW)
