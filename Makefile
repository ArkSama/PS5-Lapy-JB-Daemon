# Lapy JB Daemon v1.2 — multi-firmware standalone payload.
#
# Build flow :
#   - main.o + offsets_v940.o compiled from source/
#   - linked against vanilla libhijacker.a (place at extern/libhijacker/)
#   - offsets_v940.o is listed BEFORE -lhijacker so the linker resolves the
#     offsets:: namespace symbols from that object and skips libhijacker's
#     vanilla offsets.cpp.o (which would hardcode fw 9.40 only).
#
# Result : a binary that works on every fw covered by ps5-payload-sdk + kstuff
# (3.00 -> 12.00), with no fw-specific code in this repo.

PS5_HOST ?= 192.168.1.28
PS5_PORT ?= 9021

ifndef PS5_PAYLOAD_SDK
    $(error PS5_PAYLOAD_SDK undefined. Set to your ps5-payload-sdk path)
endif

include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

ELF := lapy_jb_daemon.elf

LIBHIJACKER_DIR := extern/libhijacker
LIBHIJACKER_INC := $(LIBHIJACKER_DIR)/include

CXXFLAGS := -std=c++20 -Wall -O2 -fno-rtti -fno-exceptions
CXXFLAGS += -I$(LIBHIJACKER_INC) -Isource

LDFLAGS		:= -nostdlib++ -Lextern/libhijacker -lhijacker -lkernel_sys -lSceNotification
LDLIBS  := -lhijacker -lkernel_sys -lSceNotification

OBJS := source/main.o source/offsets_v940.o

all: $(ELF)

source/%.o: source/%.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# offsets_v940.o is listed BEFORE -lhijacker in the link line so its symbols
# satisfy libhijacker's offsets:: refs before the archive's offsets.cpp.o
# can be pulled in.
$(ELF): $(OBJS) $(LIBHIJACKER_DIR)/libhijacker.a
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS) $(LDFLAGS) $(LDLIBS)

clean:
	rm -f $(ELF) $(OBJS)

.PHONY: all clean
