CC ?= gcc
BIN_DIR ?= bin
SRC_DIR ?= src
INC_DIR ?= src
TARGET := $(BIN_DIR)/linkstay
PREFIX ?= /usr/local
DESTDIR ?=
BINDIR ?= $(PREFIX)/bin
SYSCONFDIR ?= /etc
SYSTEMD_UNIT_DIR ?= $(SYSCONFDIR)/systemd/system
INSTALL ?= install
RM ?= rm -f
BIN ?= $(TARGET)

WARN_CFLAGS := -Wall -Wextra -Wpedantic \
	-Wshadow -Wnull-dereference -Wdouble-promotion \
	-Werror=implicit-function-declaration \
	-Werror=format-security -Wformat=2 -Wstrict-overflow=5

OPT_CFLAGS := -O3 -flto=auto -pipe

CODEGEN_CFLAGS := -ffunction-sections -fdata-sections -fmerge-all-constants \
	-fno-plt -fno-semantic-interposition -fno-common \
	-fvisibility=hidden

CFLAGS ?= $(WARN_CFLAGS) $(OPT_CFLAGS) $(CODEGEN_CFLAGS)

REQUIRED_CFLAGS := -std=c23 -fstack-protector-strong -fPIE \
                   -D_FORTIFY_SOURCE=3 \
                   -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
                   -I$(INC_DIR)

ifneq ($(PORTABLE),1)
  CFLAGS += -march=native -mtune=native
endif

CFLAGS += -MMD -MP

LDFLAGS ?= -Wl,-z,relro,-z,now -Wl,-z,noexecstack -pie -flto=auto \
           -Wl,--gc-sections -Wl,--as-needed -Wl,-O2 -Wl,--hash-style=gnu

SRCS := $(wildcard $(SRC_DIR)/*.c)
OBJS := $(patsubst $(SRC_DIR)/%.c,$(BIN_DIR)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

SAN ?= address,undefined
SANITIZE_BIN_DIR ?= bin-sanitize

.PHONY: all clean release lint test sanitize sanitize-test install install-systemd uninstall

all: $(TARGET)

release: $(TARGET)
	strip --strip-all $(TARGET)
	@echo "Release build complete (stripped): $(TARGET)"

$(BIN_DIR):
	@mkdir -p $(BIN_DIR)

$(BIN_DIR)/%.o: $(SRC_DIR)/%.c | $(BIN_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(REQUIRED_CFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) -o $@
	@echo "Build complete: $(TARGET)"

lint:
	@echo "==> Linting code..."
	@cppcheck --enable=all --suppress=missingIncludeSystem -I$(INC_DIR) $(SRC_DIR)/*.c
	@clang-tidy $(SRC_DIR)/*.c -- $(CFLAGS) $(REQUIRED_CFLAGS)

test: $(TARGET)
	BIN="$(BIN)" bash tests/run_tests.sh

# Debug-optimized build with ASan/UBSan instrumentation in a separate output
# directory (bin-sanitize/), so it never mixes object files with the normal
# optimized build. Override SAN to select sanitizers, e.g. `make sanitize
# SAN=address,undefined,leak`.
sanitize:
	$(MAKE) BIN_DIR=$(SANITIZE_BIN_DIR) \
		CFLAGS="$(WARN_CFLAGS) -O1 -g -fno-omit-frame-pointer $(CODEGEN_CFLAGS) -fsanitize=$(SAN) -fno-sanitize-recover=all" \
		LDFLAGS="-fsanitize=$(SAN) -fno-sanitize-recover=all" \
		all
	@echo "Sanitizer build complete (SAN=$(SAN)): $(SANITIZE_BIN_DIR)/linkstay"

sanitize-test: sanitize
	BIN="$(SANITIZE_BIN_DIR)/linkstay" bash tests/run_tests.sh

install: $(TARGET)
	$(INSTALL) -D -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/linkstay

install-systemd: install
	$(INSTALL) -D -m 644 systemd/linkstay.service $(DESTDIR)$(SYSTEMD_UNIT_DIR)/linkstay.service

uninstall:
	$(RM) $(DESTDIR)$(BINDIR)/linkstay
	$(RM) $(DESTDIR)$(SYSTEMD_UNIT_DIR)/linkstay.service

clean:
	rm -rf $(BIN_DIR) $(SANITIZE_BIN_DIR)

-include $(DEPS)
