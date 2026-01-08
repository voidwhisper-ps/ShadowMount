PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

# Standard Flags (No extra libraries)
CFLAGS := -O2 -Wall -D_BSD_SOURCE -std=gnu11 -Isrc -I$(INCDIR)

# Linker
LDFLAGS := -L$(LIBdir)

# Standard Libraries Only
LIBS := -lkernel_sys -lSceSystemService -lSceUserService -lSceAppInstUtil

# Targets
all: shadowmount.elf kill.elf

# Build Daemon
shadowmount.elf: src/main.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBS)

# Build Kill Switch
kill.elf: src/kill.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBS)

clean:
	rm -f shadowmount.elf kill.elf src/*.o