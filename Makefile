# vfio-user EDU Device Makefile
#
# Usage:
#   1) If libvfio-user is installed to /usr/local:
#        make
#
#   2) If using a local source tree (no install needed):
#        make VFIO_USER_SRCDIR=/path/to/libvfio-user
#
# Run:
#   ./edu_device -s /tmp/edu.sock

CC := gcc

# ---- libvfio-user location ----
# Option A: Point to source tree (no install needed)
#   make VFIO_USER_SRCDIR=/home/leo/leo/workspace/vfio-user/libvfio-user
#
# Option B: Point to installed location (default)
#   Headers in $(VFIO_USER_INCDIR), lib in $(VFIO_USER_LIBDIR)

ifdef VFIO_USER_SRCDIR
  VFIO_USER_INCDIR := $(VFIO_USER_SRCDIR)/include
  VFIO_USER_LIBDIR := $(VFIO_USER_SRCDIR)/build/lib
else
  VFIO_USER_INCDIR ?= /usr/local/include/vfio-user
  VFIO_USER_LIBDIR ?= /usr/local/lib
endif

# ---- Compiler / Linker flags ----
CFLAGS  := -Wall -Wextra -O2 -g -std=gnu11
CFLAGS  += -I$(VFIO_USER_INCDIR)

LDFLAGS := -L$(VFIO_USER_LIBDIR)
LDFLAGS += -lvfio-user -lpthread
LDFLAGS += -Wl,-rpath,$(VFIO_USER_LIBDIR)

# ---- Build targets ----
TARGET := edu_device
SRCS   := edu_device.c
OBJS   := $(SRCS:.c=.o)

.PHONY: all clean help check-lib

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)
	@echo ""
	@echo "=== Build OK: ./$(TARGET) ==="
	@echo ""
	@echo "Run:  ./$(TARGET) -s /tmp/edu.sock"

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJS)

# Verify libvfio-user can be found
check-lib:
	@echo "Include dir: $(VFIO_USER_INCDIR)"
	@echo "Library dir: $(VFIO_USER_LIBDIR)"
	@ls $(VFIO_USER_INCDIR)/libvfio-user.h 2>/dev/null \
		&& echo "[OK] Header found" \
		|| echo "[FAIL] libvfio-user.h not found in $(VFIO_USER_INCDIR)"
	@ls $(VFIO_USER_LIBDIR)/libvfio-user.so* 2>/dev/null \
		&& echo "[OK] Library found" \
		|| echo "[FAIL] libvfio-user.so not found in $(VFIO_USER_LIBDIR)"

help:
	@echo "Targets:"
	@echo "  all        - Build edu_device (default)"
	@echo "  clean      - Remove build artifacts"
	@echo "  check-lib  - Verify libvfio-user paths"
	@echo "  help       - Show this help"
	@echo ""
	@echo "Variables:"
	@echo "  VFIO_USER_SRCDIR  - Path to libvfio-user source tree"
	@echo "                      (auto-sets INCDIR and LIBDIR)"
	@echo "  VFIO_USER_INCDIR  - Path to headers (containing libvfio-user.h)"
	@echo "  VFIO_USER_LIBDIR  - Path to libvfio-user.so"
	@echo ""
	@echo "Examples:"
	@echo "  make VFIO_USER_SRCDIR=~/libvfio-user"
	@echo "  make VFIO_USER_INCDIR=/opt/include VFIO_USER_LIBDIR=/opt/lib"
