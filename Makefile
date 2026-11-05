ifdef PS4_PAYLOAD_SDK
    include $(PS4_PAYLOAD_SDK)/toolchain/orbis.mk
else
    $(error PS4_PAYLOAD_SDK is undefined)
endif

.RECIPEPREFIX = >

PAYLOAD := aviso.elf
SRCS    := main.c
CFLAGS  := -Wall -g
LDLIBS  ?=

all: $(PAYLOAD)

%.elf: $(SRCS)
> $(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

clean:
> -rm -f *.o *.elf
