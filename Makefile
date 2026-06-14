CC ?= gcc
BIN_DIR ?= bin
SRC_DIR ?= src
TARGET := $(BIN_DIR)/LinkStay

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
                   -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE

ifneq ($(PORTABLE),1)
  CFLAGS += -march=native -mtune=native
endif

CFLAGS += -MMD -MP

LDFLAGS ?= -Wl,-z,relro,-z,now -Wl,-z,noexecstack -pie -flto=auto \
           -Wl,--gc-sections -Wl,--as-needed -Wl,-O2 -Wl,--hash-style=gnu

SRCS := $(wildcard $(SRC_DIR)/*.c)
OBJS := $(patsubst $(SRC_DIR)/%.c,$(BIN_DIR)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

.PHONY: all clean release lint

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
	@cppcheck --enable=all --suppress=missingIncludeSystem $(SRC_DIR)/*.c
	@clang-tidy $(SRC_DIR)/*.c -- $(CFLAGS) $(REQUIRED_CFLAGS)

clean:
	rm -rf $(BIN_DIR)

-include $(DEPS)
