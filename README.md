# trade-engine — a Binance trading bot built around a safe, durable execution engine (C)

An autonomous trading bot for the **Binance testnet**, written in C. It watches the BTCUSDT
price, runs a moving-average crossover strategy, and routes every order through a single
execution engine.

I built it to understand how real trading systems are engineered — so the strategy isn't the
interesting part. The interesting part is everything that has to be true for an order to be
placed *correctly*: it can't be lost, it can't be sent twice, and it can't break a risk limit.
That's the engine. The strategy is a simple placeholder bolted on top to drive it.

> Testnet only — paper money, nothing real at risk. The goal was to learn the engineering, not
> to find alpha.

---

## What it does

A new price comes in every couple of seconds. The bot keeps two running averages of recent
prices — a fast one (last 3) and a slow one (last 10) — and when the fast crosses above the slow
it signals a BUY; when it crosses below, a SELL.

Every signal is handed to `place_order()`, the single path to the exchange. One call runs three
safety gates (kill-switch → exchange rules → position cap), records the order to the database,
signs and sends it, then logs whether it filled or was rejected. The next two sections break
that down.

## Sample run

A real session against the testnet — the bot warms up, detects a crossover, and the order goes
`PENDING → FILLED`:

```
$ ./trade
[strategy] live: fast=3 slow=10 poll=2s qty=0.0010. Ctrl-C or `touch KILL` to stop.
[warmup] price 59609.99  (1/10 samples)
[warmup] price 59610.00  (2/10 samples)
...                                                  # collect 10 samples to seed the slow average
[warmup] price 59631.97  (9/10 samples)
[tick] price 59631.96  fast 59631.97  slow 59619.38  (fast>slow)
[tick] price 59628.02  fast 59630.65  slow 59629.57  (fast>slow)
[tick] price 59628.02  fast 59629.33  slow 59631.18  (fast<slow)
[signal] bearish cross -> SELL
[db] PENDING SELL BTCUSDT 0.00100000 (ord-1782669364109-0434)
[db] FILLED ord-1782669364109-0434 (exchange_order_id=10217084)
...                                                  # keeps polling; trades again on the next cross
[tick] price 59615.04  fast 59615.04  slow 59613.09  (fast>slow)
[signal] bullish cross -> BUY
[db] PENDING BUY BTCUSDT 0.00100000 (ord-1782669401417-3410)
[db] FILLED ord-1782669401417-3410 (exchange_order_id=10217254)
^C
```

Every order is durably recorded, so the database is a full audit log of what the bot did:

```
$ sqlite3 -header -column trading.db \
    "SELECT side, status, quantity, exchange_order_id FROM orders
     ORDER BY created_at_ms DESC LIMIT 4;"

side  status  quantity  exchange_order_id
----  ------  --------  -----------------
BUY   FILLED  0.001     10217254
SELL  FILLED  0.001     10217084
BUY   FILLED  0.001     10208678
BUY   FILLED  0.001     10198980
```

## Design: strategy vs. engine

The codebase is split along one deliberate line:

- **The strategy decides _when_ to trade.** It's a toy on purpose — a crossover on noisy 2-second
  data will whipsaw. Swapping it out changes nothing about safety.
- **The engine decides _how_ to trade, safely.** `place_order(db, key, secret, symbol, side, qty)`
  is the only path to the exchange. Safety isn't the caller's responsibility — it's baked into the
  engine, so *any* strategy gets the safe path for free.

This separation is the main idea: a careless strategy still cannot lose an order, double-send, or
exceed a risk limit, because it physically can't reach the exchange except through the engine.

## The three safety gates

Every order passes three gates, in order, before anything is sent:

1. **Kill-switch** — an emergency stop. If a file named `KILL` exists, the engine refuses to trade.
   An operator can halt all order flow instantly with `touch KILL` — no restart, no redeploy. The
   strategy loop checks it too, so the bot also shuts itself down.
2. **Exchange-rule validation** — the engine fetches the live `LOT_SIZE` filter from the exchange
   and checks the order's quantity against the min / max / step rules *before* sending, so the
   exchange never rejects us for a malformed size.
3. **Position cap** — a hard risk limit. The engine sums filled BUY quantity from the local DB and
   refuses any BUY that would push the net position past `MAX_POSITION`.

## Reliability: idempotency & durable state

The hard problem in execution is the gap between "I sent the order" and "I know what happened to
it." If the process crashes in that window, you must not lose the order or accidentally send it
twice. This is handled with two mechanisms:

- **Persist before send.** Every order is written to SQLite as `PENDING` *before* the network call.
  A crash mid-flight leaves a durable record to reconcile against — the order is never silently lost.
- **Idempotency key + structural guard.** Each order gets a unique `client_order_id`, stored as the
  table's `PRIMARY KEY` and sent to the exchange as `newClientOrderId`. A retry with the same id
  fails the `INSERT` with a `PRIMARY KEY` constraint violation — so the engine *cannot* double-send.
  The guarantee is structural (the database enforces it), not a hope that the code is careful.

After the exchange replies, the row transitions `PENDING → FILLED` (recording the exchange's order
id) or `→ REJECTED`. The DB is the source of truth for what the system has actually done.

## Security

- API key and secret are read from environment variables (`BINANCE_API_KEY`, `BINANCE_API_SECRET`),
  never hardcoded or committed.
- Requests are signed with **HMAC-SHA256**: the secret is mixed into a hash of the request, and only
  the signature is sent — never the secret itself. The exchange recomputes it to verify the request
  is authentic and untampered.
- A millisecond `timestamp` on every request gives replay protection (the exchange rejects stale
  requests), and the network response buffer is bounds-checked against overflow.

## Tech stack

| Concern            | Library      | Why                                              |
| ------------------ | ------------ | ------------------------------------------------ |
| HTTP(S) to exchange| libcurl      | REST calls to the Binance testnet API            |
| Request signing    | OpenSSL      | HMAC-SHA256 authentication                        |
| Durable state      | SQLite       | order log, idempotency guard, position tracking  |
| JSON parsing       | cJSON        | decode the exchange's replies                     |

REST polling was chosen over a WebSocket feed to keep the focus on the execution layer; a streaming
price feed is the natural next upgrade (see below).

## Build & run

Requires the libraries above (on macOS: `brew install curl openssl@3 sqlite cjson`).

```sh
make                              # builds ./trade

export BINANCE_API_KEY=...        # your Binance testnet keys
export BINANCE_API_SECRET=...

./trade                           # start the bot
touch KILL                        # emergency stop (or Ctrl-C)
```

## Project layout

```
trade.c      the whole program: market data, signing, the 3 gates, the engine, the strategy loop
Makefile     build
trading.db   local SQLite order log (created on first run; gitignored)
```

`trade.c` is organized into labelled sections — response capture · market data · request signing ·
time & id helpers · safety gates · execution engine · strategy · main.

## What I'd build next

- **WebSocket price feed** to replace REST polling — lower latency, real streaming market data.
- **Reconciliation on startup** — query the exchange for any `PENDING` rows left by a crash and
  resolve them, closing the persist-before-send loop.
