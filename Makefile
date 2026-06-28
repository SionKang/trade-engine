# ---- build settings (reusable variables) ----
CC       = cc
CFLAGS   = -Wall
INCLUDES = -I/opt/homebrew/opt/openssl@3/include -I/opt/homebrew/include
LIBDIRS  = -L/opt/homebrew/opt/openssl@3/lib -L/opt/homebrew/lib
LIBS     = -lsqlite3 -lcurl -lcrypto -lcjson -lm

# ---- default target: build `trade` from trade.c ----
trade: trade.c
	$(CC) $(CFLAGS) $(INCLUDES) $(LIBDIRS) trade.c $(LIBS) -o trade

# ---- remove the built binary ----
clean:
	rm -f trade
