# ---- build settings ----
# pkg-config finds each library wherever it is installed (Homebrew on macOS, the
# system on Linux), so no machine-specific path is hardcoded here.
CC     = cc
PKGS   = libcurl libcrypto sqlite3 libcjson
CFLAGS = -std=gnu11 -Wall -Wextra -Wpedantic -O2 $(shell pkg-config --cflags $(PKGS))
LIBS   = $(shell pkg-config --libs $(PKGS)) -lm

# ---- default target: build `trade` from trade.c ----
trade: trade.c
	$(CC) $(CFLAGS) trade.c $(LIBS) -o trade

# ---- remove the built binary (never trading.db) ----
clean:
	rm -f trade
