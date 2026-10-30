# linkstay build entry points: build, release, test, unit-test, lint,
# shellcheck, check, sanitize and install. The engineering contract for changes
# lives in .github/copilot-instructions.md.
CC ?= gcc
BIN_DIR ?= bin
SRC_DIR ?= src
INC_DIR ?= $(SRC_DIR)
TEST_DIR ?= tests
TARGET := $(BIN_DIR)/linkstay
PREFIX ?= /usr/local
DESTDIR ?=
BINDIR ?= $(PREFIX)/bin
SYSCONFDIR ?= /etc
SYSTEMD_UNIT_DIR ?= $(SYSCONFDIR)/systemd/system
CONFIG_FILE ?= $(SYSCONFDIR)/linkstay.conf
INSTALL ?= install
RM ?= rm -f
STRIP ?= strip
WARN_CFLAGS := -Wall -Wextra -Wpedantic \
	-Wshadow -Wnull-dereference -Wdouble-promotion \
	-Wconversion -Wsign-conversion -Wundef \
	-Werror=implicit-function-declaration \
	-Werror=format-security -Wformat=2 -Wstrict-overflow=5

OPT_CFLAGS := -O2 -pipe

CODEGEN_CFLAGS := -ffunction-sections -fdata-sections -fno-plt

CFLAGS ?= $(WARN_CFLAGS) $(OPT_CFLAGS) $(CODEGEN_CFLAGS)

# -pedantic-errors keeps the sources strictly C23: the only tolerated vendor
# extensions are [[gnu::...]] attributes (valid C23 attribute syntax) and the
# GCC/Clang __builtin_expect used by base.h. The C23 requirement itself is
# enforced by an #error in base.h.
REQUIRED_CFLAGS := -std=c23 -pedantic-errors -fstack-protector-strong -fPIE \
                   -D_FORTIFY_SOURCE=3 \
                   -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
                   -I$(INC_DIR)

# Native tuning is opt-in: the default build should be deployable on other
# machines, not just the build host.
NATIVE ?= 0
ifeq ($(NATIVE),1)
  CFLAGS += -march=native -mtune=native
endif

# Link-time optimization is always on; the supported GCC/Clang toolchains
# provide it (the sanitizer build overrides CFLAGS/LDFLAGS and stays non-LTO).
CFLAGS += -flto

CFLAGS += -MMD -MP

LDFLAGS ?= -Wl,-z,relro,-z,now -Wl,-z,noexecstack -pie \
           -Wl,--gc-sections -Wl,--as-needed -flto

SRCS := $(wildcard $(SRC_DIR)/*.c)
OBJS := $(patsubst $(SRC_DIR)/%.c,$(BIN_DIR)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

# Stamped so switching NATIVE/flags rebuilds objects instead of silently
# relinking stale ones. The stamp is rewritten only when its content changes.
CONFIG_STAMP := $(BIN_DIR)/.build-config

UNIT_TEST := $(BIN_DIR)/unit-tests
UNIT_SRCS := $(TEST_DIR)/unit.c $(SRC_DIR)/base.c $(SRC_DIR)/addr.c \
             $(SRC_DIR)/log.c $(SRC_DIR)/config.c $(SRC_DIR)/stats.c
UNIT_HEADERS := $(INC_DIR)/base.h $(INC_DIR)/addr.h \
                $(INC_DIR)/log.h $(INC_DIR)/config.h $(INC_DIR)/stats.h
UNIT_CFLAGS := $(filter-out -MMD -MP,$(CFLAGS))

SAN ?= address,undefined
SANITIZE_BIN_DIR ?= bin-sanitize
SAN_CFLAGS := $(WARN_CFLAGS) -O1 -g -fno-omit-frame-pointer \
              $(CODEGEN_CFLAGS) -fsanitize=$(SAN) -fno-sanitize-recover=all
SAN_LDFLAGS := -fsanitize=$(SAN) -fno-sanitize-recover=all

.PHONY: all clean release lint shellcheck test unit-test check sanitize \
        sanitize-unit-test sanitize-test install uninstall \
        help FORCE

all: $(TARGET)

help:
	@printf 'linkstay build targets:\n'
	@printf '  make                 build bin/linkstay\n'
	@printf '  make release         build and strip bin/linkstay\n'
	@printf '  make test            unit tests + shell suite\n'
	@printf '  make unit-test       build and run tests/unit.c\n'
	@printf '  make lint            cppcheck + clang-tidy\n'
	@printf '  make shellcheck      shellcheck the test suite\n'
	@printf '  make check           test + lint + shellcheck + sanitize-test\n'
	@printf '  make sanitize        ASan/UBSan build into bin-sanitize/\n'
	@printf '  make sanitize-test   sanitizer build + both test suites\n'
	@printf '  make install         install binary, unit, and /etc config\n'
	@printf '  make uninstall       remove binary and unit, preserve config\n'
	@printf '  make clean           remove build directories\n'
	@printf 'Variables: PREFIX=/usr/local, CONFIG_FILE=/etc/linkstay.conf,\n'
	@printf '           NATIVE=1 -march=native, SAN=<list>\n'

release: $(TARGET)
	$(STRIP) --strip-all $(TARGET)
	@echo "Release build complete (stripped): $(TARGET)"

$(BIN_DIR):
	@mkdir -p $(BIN_DIR)

$(CONFIG_STAMP): FORCE | $(BIN_DIR)
	@printf '%s\n' "$(CC)" "$(CFLAGS) $(REQUIRED_CFLAGS)" "$(LDFLAGS)" > "$@.tmp"
	@if cmp -s "$@.tmp" "$@"; then rm -f "$@.tmp"; else mv "$@.tmp" "$@"; fi

FORCE:

$(BIN_DIR)/%.o: $(SRC_DIR)/%.c $(CONFIG_STAMP) | $(BIN_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(REQUIRED_CFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) -o $@
	@echo "Build complete: $(TARGET)"

$(UNIT_TEST): $(UNIT_SRCS) $(UNIT_HEADERS) $(CONFIG_STAMP) | $(BIN_DIR)
	$(CC) $(UNIT_CFLAGS) $(REQUIRED_CFLAGS) $(UNIT_SRCS) $(LDFLAGS) -o $@

unit-test: $(UNIT_TEST)
	$(UNIT_TEST)

lint:
	@echo "==> Linting code..."
	@cppcheck --enable=all --suppress=missingIncludeSystem \
		--suppress=constParameterCallback -I$(INC_DIR) $(SRC_DIR)/*.c
	@clang-tidy $(SRC_DIR)/*.c \
		--checks=-clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling \
		-- $(CFLAGS) $(REQUIRED_CFLAGS)

shellcheck:
	@echo "==> ShellCheck..."
	@shellcheck -S warning $(TEST_DIR)/*.sh

test: unit-test $(TARGET)
	BIN="$(TARGET)" bash $(TEST_DIR)/run_tests.sh

# Everything a change must pass before it is considered done (see
# .github/copilot-instructions.md).
check: test lint shellcheck sanitize-test

# Sanitizer build: -O1 with debug info plus ASan/UBSan instrumentation, in a
# separate output directory (bin-sanitize/) so it never mixes object files with
# the normal optimized build. Override SAN to select sanitizers, e.g.
# `make sanitize SAN=address,undefined,leak`.
sanitize:
	$(MAKE) BIN_DIR=$(SANITIZE_BIN_DIR) \
		CFLAGS="$(SAN_CFLAGS) -MMD -MP" \
		LDFLAGS="$(SAN_LDFLAGS)" \
		all
	@echo "Sanitizer build complete (SAN=$(SAN)): $(SANITIZE_BIN_DIR)/linkstay"

sanitize-unit-test: sanitize
	$(MAKE) BIN_DIR=$(SANITIZE_BIN_DIR) \
		CFLAGS="$(SAN_CFLAGS) -MMD -MP" \
		LDFLAGS="$(SAN_LDFLAGS)" \
		unit-test

sanitize-test: sanitize-unit-test
	BIN="$(SANITIZE_BIN_DIR)/linkstay" bash $(TEST_DIR)/run_tests.sh

install: $(TARGET)
	$(INSTALL) -D -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/linkstay
	$(INSTALL) -d $(DESTDIR)$(SYSTEMD_UNIT_DIR)
	sed -e 's|@BINDIR@|$(BINDIR)|g' \
	    -e 's|@CONFIG_PATH@|$(CONFIG_FILE)|g' \
	    systemd/linkstay.service > \
	    $(DESTDIR)$(SYSTEMD_UNIT_DIR)/linkstay.service
	chmod 644 $(DESTDIR)$(SYSTEMD_UNIT_DIR)/linkstay.service
	@if [ ! -e "$(DESTDIR)$(CONFIG_FILE)" ]; then \
		$(INSTALL) -D -m 644 systemd/linkstay.conf \
			"$(DESTDIR)$(CONFIG_FILE)"; \
	else \
		echo "Preserving existing $(DESTDIR)$(CONFIG_FILE)"; \
	fi
	@echo "Run: systemctl daemon-reload && systemctl enable --now linkstay"

uninstall:
	$(RM) $(DESTDIR)$(BINDIR)/linkstay
	$(RM) $(DESTDIR)$(SYSTEMD_UNIT_DIR)/linkstay.service
	@echo "Preserved configuration: $(DESTDIR)$(CONFIG_FILE)"
	@echo "Run: systemctl daemon-reload"

clean:
	rm -rf $(BIN_DIR) $(SANITIZE_BIN_DIR)

-include $(DEPS)
