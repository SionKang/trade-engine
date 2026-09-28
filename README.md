# trade-engine — a Binance trading bot built around a safe, durable execution engine (C)

An autonomous trading bot for the **Binance Spot testnet**, written in C. It watches the BTCUSDT
price, runs a moving-average crossover strategy, and routes every order through a single execution
engine.

The strategy is deliberately a toy. The interesting part is everything that has to be true for an
order to be placed *correctly*: it is recorded before it is sent, it is checked against the
exchange's own size rules and a risk limit, and when the outcome is unclear the program says so
instead of guessing. Then it measures where the time goes.

> Testnet only — paper money, nothing real at risk. The goal was to learn the engineering, not to
> find alpha.

## At a glance

- **Never claims more than the exchange proved.** Every order ends `FILLED`, `FAILED` (only when
  something proves no order exists) or `UNKNOWN`. A Binance `-1007` timeout or an HTTP 5XX is
  `UNKNOWN`, never "rejected" — because the order may well have filled.
- **Persist before send.** Every order is written to SQLite *before* the network call, in WAL mode
  with full syncing. The risk gate fails **closed**: if the position can't be read, it won't trade.
- **Measured, not guessed.** A built-in benchmark (`./trade --bench N`, no credentials, no orders)
  splits latency into DNS / TCP / TLS / wait and local work, reported as p50 / p99 / max.
- **Three measured optimisations** — network p50 **101 → 47 ms**, p99 **664 → 145 ms**, database
  insert **363 → 107 µs**, round trips per order **2 → 1** — plus two that were measured and
  deliberately **declined**.
- **Honest about its limits.** No reconciliation, `double` quantities, testnet over Wi-Fi — all listed
  below with the reasoning, rather than hidden.

---

## What it does

A new price comes in every couple of seconds. The bot keeps two running averages of recent prices —
a fast one (last 3) and a slow one (last 10) — and when the fast crosses above the slow it signals a
BUY; when it crosses below, a SELL.

Every signal is handed to `place_order()`, the single path to the exchange. One call runs three
safety gates (kill-switch → exchange rules → position cap), records the order to the database,
signs and sends it, then classifies what came back as `FILLED`, `FAILED` or `UNKNOWN`.

## Sample run

Startup on the current build — exchange rules are read once, then the strategy goes live:

```
$ ./trade
[rules] BTCUSDT LOT_SIZE min 0.00001000 max 9000.00000000 step 0.00001000 (fetched once)
[strategy] live: fast=3 slow=10 poll=2s qty=0.0010. Ctrl-C or `touch KILL` to stop.
```

A trading session, captured on an earlier build. The bot warms up, detects a crossover, and the
order goes `PENDING → FILLED` (current builds also print a `[lat]` line after each order, splitting
its time into local work before sending, the HTTP round trip, the work after the reply, and the total):

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

## Design: strategy vs. engine

- **The strategy decides _when_ to trade.** It's a toy on purpose — a crossover on noisy 2-second
  data will whipsaw. Swapping it out changes nothing about safety.
- **The engine decides _how_ to trade, safely.** `place_order()` is the only path to the exchange.
  Safety isn't the caller's responsibility — it's in the engine, so *any* strategy gets it.

A careless strategy cannot skip the risk limit, the exchange's size rules, or the durable record,
because it physically can't reach the exchange except through the engine.

## The three safety gates

Every order passes three gates, in order, before anything is sent:

1. **Kill-switch** — if a file named `KILL` exists, the engine refuses to trade. An operator halts all
   order flow with `touch KILL`: no restart, no redeploy. The strategy loop checks it too, so the bot
   also shuts itself down.
2. **Exchange-rule validation** — the quantity is checked against the symbol's `LOT_SIZE`
   min / max / step rules. The filters are fetched **once at startup**, so this gate is entirely local,
   and startup refuses a symbol whose status isn't `TRADING`. It checks `LOT_SIZE` **only**: testnet
   BTCUSDT also publishes `MARKET_LOT_SIZE` and a `NOTIONAL` filter with a 5 USDT market-order
   minimum, so the exchange can still reject a size this gate accepts.
3. **Position cap** — the net position (filled BUYs minus filled SELLs) may not exceed
   `MAX_POSITION` after a BUY. It **fails closed**: if the position query errors, the order is refused
   rather than treated as a flat position. The cap only checks BUYs: a SELL is sent without checking
   whether the bot actually holds that much BTC. The exchange only rejects it if the whole account is
   short, so a SELL can go through using BTC the bot never bought.

## Reliability: what's guaranteed and what isn't

The hard problem in execution is the gap between "I sent the order" and "I know what happened to it."

**What it does give you:**

- **Persist before send.** Every order is written as `PENDING` *before* the network call, so an order
  can never be silently lost. A crash mid-flight leaves the `PENDING` row behind; a timeout is
  classified `UNKNOWN` rather than guessed at.
- **A name for every order.** Each order gets a unique `client_order_id`, stored as the `PRIMARY KEY`
  and sent as `newClientOrderId`. Its job is **identification** — it's the handle for asking the
  exchange what happened to an order you lost track of.

**What it does not give you:**

- **This is not an idempotency key.** The id is generated *per call*, so the `PRIMARY KEY` can only
  reject an insert if two calls land in the same millisecond *and* draw the same random suffix. It
  prevents recording the same name twice; it does nothing to prevent two orders with different names.
- **The exchange doesn't treat it as permanent either.** Binance documents `newClientOrderId` as unique
  *among open orders* — "orders with the same `newClientOrderID` can be accepted only when the
  previous one is filled". Once an order fills, its id is reusable.
- **No id scheme solves the underlying problem.** If a request times out, nothing on the client side
  can tell whether the exchange accepted it. That's answered by *asking*, not by retrying.

A row ends in exactly one of four states, and two of them require proof:

| Status | Meaning | Written when |
| --- | --- | --- |
| `PENDING` | inserted, not yet sent | before the network call |
| `FILLED` | the exchange confirmed the fill | HTTP 200 and `status` is `FILLED` |
| `FAILED` | no order exists at the exchange | nothing was transmitted (DNS/TCP/TLS failed), or a 4XX carried a Binance error code proving refusal |
| `UNKNOWN` | the request may have reached the matching engine | everything else |

`UNKNOWN` is the default, not the exception: `-1007` ("Send status unknown; execution status
unknown"), any HTTP 5XX — which the docs say explicitly *not* to treat as failure — a dropped
connection after transmission, an unparseable body, or a live order state like `NEW`. "Never sent" is
told apart from "sent, no answer" using libcurl's `CURLINFO_PRETRANSFER_TIME`, which stays zero until
bytes are about to go on the wire.

Writing `FAILED` where the truth is `UNKNOWN` is the dangerous direction: the database would contradict
reality for an order that actually filled, and the position cap would under-count real exposure.

**The exchange is the source of truth.** The database is this bot's record of what it *tried* to do
and what it *last heard back*. Treating it as authoritative would mean asserting knowledge the program
doesn't have.

---

## Performance

### How it's measured

`./trade --bench N` reads the public price endpoint N times and runs the local per-order work N times
in isolation. It places **no orders** and needs **no credentials**. For every request it records
libcurl's own timing breakdown; for the local work it times the HMAC signature and the exact SQLite
statements the bot runs, against a real file on the same disk. Durations are integer nanoseconds from
`CLOCK_MONOTONIC`. Each percentile is a real sample from the sorted list, the one at index
⌊p·(n−1)/100⌋ — the lower of the two samples an interpolated percentile would fall between. Full history, including
every before/after run, is in **[bench/baseline.md](bench/baseline.md)**.

### Final numbers

Commit `5b9ff92`, N = 300, `-O2`, Apple M2 Pro, macOS, Wi-Fi from Seoul to the Binance testnet.

**Network — warm requests (2..300), reusing one connection:**

| stage | min | p50 | p99 | max |
| --- | ---: | ---: | ---: | ---: |
| DNS | 0.01 ms | 0.04 ms | 0.08 ms | 0.16 ms |
| TCP connect | 0.00 ms | 0.00 ms | 0.00 ms | 0.00 ms |
| TLS handshake | 0.00 ms | 0.00 ms | 0.00 ms | 0.00 ms |
| wait (sent → first byte) | 38.25 ms | 46.76 ms | 144.46 ms | 167.30 ms |
| **total** | **38.37 ms** | **46.96 ms** | **144.59 ms** | **167.43 ms** |

The first request of the same run, **cold**: DNS 3.35, TCP 30.15, TLS 30.58 and wait 49.70 ms —
**113.92 ms** in total. Same network, same minute; the difference is the 64 ms of DNS + TCP + TLS
setup that a warm request never pays.

**Local work, per order:**

| operation | min | p50 | p99 | max | on the path to market? |
| --- | ---: | ---: | ---: | ---: | --- |
| HMAC-SHA256 signature | 4 µs | 5 µs | 13 µs | 17 µs | yes |
| SQLite INSERT (persist before send) | 76 µs | 107 µs | 2707 µs | 4146 µs | yes |
| SQLite UPDATE (record outcome) | 43 µs | 62 µs | 179 µs | 2584 µs | no — after the send |

### What changed, measured before and after

| | baseline | final | change |
| --- | ---: | ---: | --- |
| network total, p50 | 101.0 ms | **47.0 ms** | one reused connection |
| network total, p99 | 663.6 ms | **144.6 ms** | |
| network total, max | 1609.3 ms | **167.4 ms** | |
| DNS + TCP + TLS, p50 | 48.4 ms | **0.04 ms** | |
| round trips per order | 2 | **1** | exchange rules fetched once at startup |
| rate-limit weight per order | 21 | **1** | |
| SQLite INSERT, p50 | 363 µs | **107 µs** | WAL journal mode |

Network conditions differ between runs, so `wait` — the real round trip — is the control. It sat at
48 ms in the baseline and 47 ms in the final run, which is what makes the comparison fair: the round
trip didn't change, the setup around it did.

**Measured and deliberately declined:**

- **`synchronous=NORMAL`** would make the INSERT ~5× faster again, but an OS crash or power loss could
  then lose recent commits — exactly what writing the row *before* sending exists to prevent.
- **Reusing the prepared SQLite statement** left the median unchanged. The cost is the disk sync, not
  SQL parsing; the obvious guess was wrong.

### What this means for HFT

**1. At this deployment, the code isn't the bottleneck — and optimising it would be wasted effort.**
The two local steps on the path to market, the HMAC and the INSERT, take about **0.11 ms** of a
**47 ms** round trip: **0.24%**. Making this C twice as fast would get an order to the exchange 0.1 ms
sooner. The network is 99%+ of the time, so every optimisation that mattered here was about the network.

**2. Colocated, that ordering inverts.** A trading firm puts its machines in the exchange's data centre,
where the round trip is measured in microseconds rather than milliseconds. Take a hypothetical 50 µs
round trip: the 107 µs INSERT alone would then be **twice the network time**, and the 5 µs HMAC would
be 10% of it. The same code, unchanged, goes from "irrelevant" to "the bottleneck" purely because of
where it runs. That is why this project reports local and network latency separately instead of as one
number — a single total would hide exactly the thing that decides what to optimise.

**3. Durability and latency trade against each other, and there is no free version.** Persist-before-send
costs a synchronous disk write on every order. At 47 ms of network that is noise; colocated it would be
the dominant cost. Latency-critical systems deal with this by moving persistence off the critical path
(record in memory, write asynchronously), by getting durability from replication to another machine
instead of a local fsync, or by using storage that makes the sync cheap. Each option either weakens the
guarantee or moves it somewhere else. This project keeps the synchronous write, deliberately, and
declined the faster setting that would have broken it.

**4. The tail matters more than the median.** Network p99 is 3× the p50, and the INSERT's p99 was 25×
its p50 in the final run. In trading the slow order is the expensive one — it's the one that arrives
after the price moved. I investigated the INSERT tail rather than guessing: my first hypothesis was
SQLite's WAL checkpoints, but disabling them didn't remove the spikes, the spikes were irregular rather
than periodic, and two of three repeat runs had no tail at all. It is **fsync latency variance from the
disk and OS** — intermittent, and outside the program's control. That is the real argument against a
synchronous disk write on a latency-critical path: it puts an uncontrolled tail on every order.

**5. Do setup work before the order, not during it.** Two of the three wins came from moving work
*off* the critical path rather than making it faster. Reusing one connection means an order never pays
a DNS lookup, TCP handshake or TLS handshake — 113.92 ms cold versus 46.96 ms warm in the same run.
Fetching the exchange's rules once at startup removed an entire request from every order. The fastest
work is the work that's already been done when the signal arrives.

**What this does not show:** anything about colocated or HFT-grade latency. This is public testnet REST
over consumer Wi-Fi from Seoul. The numbers are real, but they describe a laptop, not a trading venue —
the value is in the method: measure each stage separately, change one thing, re-measure against a
control, and know which side of the network/software line you're standing on.

---

## Security

- API key and secret come from environment variables (`BINANCE_API_KEY`, `BINANCE_API_SECRET`), never
  hardcoded or committed.
- Requests are signed with **HMAC-SHA256**: the secret is mixed into a hash of the request, and only the
  signature is sent — never the secret. The exchange recomputes it to verify the request is authentic.
- A millisecond `timestamp` on every request gives replay protection, and the response buffer is
  bounds-checked against overflow.
- Every endpoint is built from one `BASE_URL` define pointing at the testnet, so "testnet only" is a
  single auditable line.

## Tech stack

| Concern | Library | Why |
| --- | --- | --- |
| HTTP(S) to the exchange | libcurl | REST calls, with per-stage timing |
| Request signing | OpenSSL | HMAC-SHA256 authentication |
| Durable state | SQLite (WAL) | order log, position tracking |
| JSON parsing | cJSON | decode the exchange's replies |

REST polling was chosen over a WebSocket feed to keep the focus on the execution layer.

## Build & run

Requires `pkg-config` plus the four libraries.

```sh
# macOS
brew install pkg-config curl openssl@3 sqlite cjson
# Debian / Ubuntu
sudo apt install pkg-config libcurl4-openssl-dev libssl-dev libsqlite3-dev libcjson-dev

make                              # builds ./trade  (-Wall -Wextra -Wpedantic -O2, zero warnings)

./trade --bench 300               # latency measurement: no credentials, no orders

export BINANCE_API_KEY=...        # your Binance testnet keys
export BINANCE_API_SECRET=...
./trade                           # start the bot
touch KILL                        # emergency stop (or Ctrl-C)
```

## Project layout

```
trade.c            the whole program
Makefile           build
bench/baseline.md  every latency measurement, before and after each change
trading.db         local SQLite order log (created on first run; gitignored)
```

`trade.c` reads top to bottom as HTTP · market data · request signing · time & ids · database ·
safety gates · execution engine · strategy · benchmark · the bot · main. About 530 lines of code and
260 of comment, in one file on purpose: the whole order path is readable in one sitting.

## Assumptions and limitations

These are deliberate. The program is correct only within them, so they're stated rather than implied.

- **No retries, anywhere.** Each crossover produces exactly one submission attempt; if it fails, the
  signal is dropped. That is the actual reason a single decision never becomes two orders — not a
  structural guarantee. Add a retry path and the property disappears with nothing to catch it.
- **No reconciliation.** Nothing ever revisits an `UNKNOWN` row, or a `PENDING` one left by a crash. If
  an order timed out and did in fact fill, the recorded position is wrong from then on.
- **The recorded position is this bot's, not the account's.** Manual trades, another client on the same
  key, fees and deposits are invisible to it.
- **Exchange rules are cached at startup and never refreshed.** Staleness fails safe in both
  directions — if filters tighten, the exchange rejects the size and the reply is classified `FAILED`;
  if they loosen, the bot is merely over-restrictive — but a mid-run change needs a restart.
- **Fully-filled orders only.** The position counts rows whose status is `FILLED`, so a partial fill
  contributes nothing. Market orders on a liquid symbol usually fill completely; not so in general.
- **Quantities are `double`s**, the wrong type for anything the exchange acts on. I tested where it
  actually bites. The wire value is fine (`%.8f` of `0.001` gives `"0.00100000"`), and the step check is
  off by `1.4e-14`, well inside its `1e-6` tolerance. The one real defect is the **position cap**: 100
  orders of 0.001 sum to `0.10000000000000007`, so the order that should land exactly on the cap is
  refused — safe direction, but wrong. The argument for `int64_t` units isn't that bug; it's that the
  honest claim today is "this works because the error happens to be smaller than a fudge factor I
  chose", where with integers it is simply `qty % step == 0`. The step check also tests
  `(qty - minQty) % stepSize` where the Binance docs specify `quantity % stepSize`; they agree for
  BTCUSDT only because `minQty` equals `stepSize`.
- **One symbol, one strategy, one process, one thread.**
- **Testnet only**, and its latency says nothing about colocated latency.

## What I'd build next

In order of how much each would improve correctness:

- **Reconciliation on startup** — query the exchange by `origClientOrderId` for every `UNKNOWN` or
  `PENDING` row and resolve it. The biggest gap: it's the only way an `UNKNOWN` ever gets settled.
- **A stable id per decision** — mint the `client_order_id` once per decision and persist it before
  the first attempt, so a future retry reuses one name instead of creating a second order.
- **Fixed-point quantities** — exact `int64` units, making the `LOT_SIZE` check an exact remainder.
- **Refresh exchange rules after a filter rejection**, instead of requiring a restart.
- **A WebSocket price feed** to replace REST polling.

## On AI assistance

The original version of this bot — the order pipeline, the safety gates and the strategy loop — I wrote myself, using Claude Code as a tutor. The later round of changes was written with Claude Code: the latency benchmark, the speed improvements it pointed to (connection reuse, WAL mode, caching the exchange rules). My part there was deciding what to measure, reading the results to find the bottleneck, and choosing what to change and what to leave alone. I understand how it all works and can walk through it — which, more than who typed it, is what I wanted to get out of this project.
