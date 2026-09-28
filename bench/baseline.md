# Latency: baseline, three optimisations, and the final build

Measured with `./trade --bench 300`. Read-only: it polls one public endpoint and
exercises the local work in isolation. No credentials, no orders.

## Environment

| | |
| --- | --- |
| date | 2026-09-28, UTC |
| machine | Apple M2 Pro, macOS 26.5.2 (arm64) |
| network | Wi-Fi (`en0`), public internet, Seoul → Binance testnet |
| compiler | Apple clang 16.0.0 — runs 1–4 built with `-Wall` only (no `-O2`); run 5 with `-O2` |
| libcurl | 8.7.1 · OpenSSL 3.6.4 · SQLite 3.51.0 |
| samples | N=300 per run |

Each percentile is a real sample from the sorted array of integer nanosecond samples:
the one at index ⌊p·(n−1)/100⌋, the lower of the two an interpolated percentile would
fall between. Every run below uses this same definition, so they compare fairly.
`wait` is request-sent → first-byte-back; libcurl reports its milestones
cumulatively, so each row is the adjacent difference, i.e. what that stage cost.

---

## Run 1 — baseline (`9112535`)

Fresh `curl_easy_init()` per request; SQLite at default journal mode.

| metric | min ms | p50 ms | p99 ms | max ms |
| --------- | ------ | ------ | ------ | ------ |
| dns       |   1.88 |   3.38 |   5.07 |  14.61 |
| tcp       |  25.90 |  30.25 | 468.91 | 1543.75 |
| tls       |  10.76 |  14.76 | 321.03 | 380.01 |
| wait      |  36.19 |  48.14 | 170.60 | 249.39 |
| total     |  81.64 | 100.98 | 663.59 | 1609.31 |

| metric | min µs | p50 µs | p99 µs | max µs |
| --------- | ------ | ------ | ------ | ------ |
| hmac      |   5.00 |   6.00 |  24.00 | 222.00 |
| insert    | 289.00 | 363.00 | 978.00 | 1474.00 |
| update    | 210.00 | 273.00 | 643.00 | 774.00 |

**Reading:** connection setup (dns + tcp + tls) is 48.4 ms of a 100.98 ms median —
**~48% of every request** — and it is re-paid each time, for a connection that was
just discarded. Local software is 6 + 363 + 273 ≈ 0.64 ms, i.e. **0.6%** of the
round trip: optimising it here would be optimising noise.

---

## Run 2 — SQLite in WAL mode

`PRAGMA journal_mode=WAL` with `synchronous=FULL`. Local writes only; the network
path is untouched.

| metric | min µs | p50 µs | p99 µs | max µs |
| --------- | ------ | ------ | ------ | ------ |
| hmac      |   4.00 |   4.00 |   5.00 |   6.00 |
| insert    |  79.00 | 100.00 | 237.00 | 327.00 |
| update    |  46.00 |  60.00 | 118.00 | 167.00 |

**insert p50 363 → 100 µs (3.6×), p99 978 → 237 µs (4.1× tighter tail).**

The default rollback journal writes a journal, syncs it, writes the database, then
syncs again. WAL appends the change to one log and syncs that once. **Identical
durability promise, half the syncs** — this is not a tradeoff, it is a better
journal design.

An isolated experiment on the same filesystem, n=300 per configuration:

| config | min µs | p50 µs | p99 µs | max µs |
| -------------------------- | ------ | ------ | ------ | ------ |
| default journal + FULL     |    281 |    337 |   1281 |   6268 |
| WAL + synchronous=FULL     |     64 |     74 |    391 |   3564 |
| WAL + synchronous=NORMAL   |     13 |     15 |     32 |    201 |
| WAL + FULL, stmt reused    |     66 |     74 |    104 |    138 |
| WAL + NORMAL, stmt reused  |     11 |     12 |     25 |    230 |

Two conclusions from that table:

- **`synchronous=NORMAL` was declined.** It is ~5× faster again, but on OS crash or
  power loss recently committed transactions can be lost — precisely what writing
  the row *before* sending exists to prevent. A process crash (`kill -9`, segfault)
  is still safe under `NORMAL` because the data sits in the OS page cache, so the
  trade is specifically "keep protection from my program dying, lose protection
  from the machine dying". Not worth 59 µs on a 100 ms round trip.
- **Reusing the prepared statement is not the win.** Median identical at 74 µs, so
  the cost is the disk sync, not SQL parsing. (The tails look tighter, but that is
  one run and maxes are noisy — not claimed.)

---

## Run 3 — one reused curl handle

One process-lifetime handle, `curl_easy_reset()` between requests. Reset clears the
options but keeps the live connection, TLS session cache and DNS cache.

| metric | min ms | p50 ms | p99 ms | max ms |
| --------- | ------ | ------ | ------ | ------ |
| dns       |   0.01 |   0.04 |   0.07 |   0.20 |
| tcp       |   0.00 |   0.00 |   0.00 |   0.00 |
| tls       |   0.00 |   0.00 |   0.00 |   0.00 |
| wait      |  41.73 |  51.83 | 155.61 | 207.82 |
| total     |  41.85 |  52.02 | 155.75 | 207.91 |

---

## Attribution

| p50 | baseline | + WAL | + reuse |
| --- | --- | --- | --- |
| dns + tcp + tls | 48.4 ms | 54.8 ms | **0.04 ms** |
| wait (control) | 48.1 ms | 51.2 ms | 51.8 ms |
| network total | 101.0 ms | 113.4 ms | **52.0 ms** |
| insert | 363 µs | **100 µs** | 110 µs |
| update | 273 µs | **60 µs** | 65 µs |

**Network conditions are not identical between runs** — the public internet moves
around, and run 2's total is *higher* than run 1's despite WAL changing nothing on
the network path. So `wait` is used as the control: it is the real round trip and
stays at 48–52 ms across all three runs, which is what makes the setup-cost
comparison meaningful. What changed is the handshake term, from 48.4 ms to 0.04 ms.

**Network total p50 101 → 52 ms, a 48% reduction. Tail p99 664 → 156 ms (4.3×),
max 1609 → 208 ms (7.7×).** The tail improved more than the median, because the
handshakes were where the worst outliers lived (baseline tcp max 1544 ms).

**`total` is now 52.02 ms against a `wait` of 51.83 ms.** A request is essentially
pure round trip: there is nothing measurable left to remove from the network path
without moving the machine. That is the honest stopping point for this deployment.

**Local work is now 5 + 110 + 65 ≈ 0.18 ms of 52 ms — 0.35%.** Still noise. Both
optimisations were worth doing, and neither was chosen because the local numbers
demanded it.

---

## Run 4 — exchange filters fetched once at startup

`exchangeInfo` used to run *inside* `place_order()`, so the order path contained
**two** round trips: fetch the filters, then send the order. The filters are now read
once at startup and the gate is purely local.

**Verified structurally rather than by timing**, because timing the order path means
placing real orders. Driving `place_order()` six times against a local mock:

| | requests received |
| --- | --- |
| before | 6 GET `exchangeInfo` + 6 POST `order` = **12** |
| after | 0 GET + 6 POST `order` = **6** |

Consequences, stated as arithmetic from run 3's measured per-request cost rather
than as a new measurement:

- **one full round trip (~52 ms) disappears from the work done before the order is
  sent.** The filter fetch sat in the gates, ahead of the POST. The `pre-send` figure
  in the `[lat]` line printed by `place_order()` will show this on the next real
  testnet order. (How long the order then takes to *reach* the exchange can't be
  measured from the client: that moment falls inside the round trip, with no clock
  on our side that can see it.)
- **rate-limit cost per order falls from 21 weight to 1.** `exchangeInfo` is
  documented at weight 20 against the order's 1, so 20/21 of an order's budget was
  buying a value that does not change. Against the 6000/minute IP limit that raises
  the ceiling from roughly 285 orders/minute to roughly 6000.
- **order placement is decoupled from market data.** Previously a failed read on
  `exchangeInfo` returned early and blocked the order; now a blip on a read-only
  endpoint cannot stop the bot trading.

This one made the code *shorter*: `place_order()` lost its fetch-and-check branch,
and `fetch_symbol_rules()` also gained a `status == TRADING` check for free, since it
was already parsing that response. It additionally fixed a latent crash — the old
parser dereferenced `minQty`/`maxQty`/`stepSize` without checking they existed.

## Run 5 — final build (`5b9ff92`, `-O2`)

After the final cleanup (shared SQL constants, fail-closed position check) and the first run
with `-O2`. These are the numbers quoted in the README.

| metric | min ms | p50 ms | p99 ms | max ms |
| --------- | ------ | ------ | ------ | ------ |
| dns       |   0.01 |   0.04 |   0.08 |   0.16 |
| tcp       |   0.00 |   0.00 |   0.00 |   0.00 |
| tls       |   0.00 |   0.00 |   0.00 |   0.00 |
| wait      |  38.25 |  46.76 | 144.46 | 167.30 |
| total     |  38.37 |  46.96 | 144.59 | 167.43 |

Cold first request, same run: dns 3.35, tcp 30.15, tls 30.58, wait 49.70, total **113.92 ms**.
This is the cleanest demonstration of connection reuse in the whole file, because it is a
*within-run* comparison — same network, same minute: 113.92 ms cold against 46.96 ms warm,
the difference being 64 ms of DNS + TCP + TLS that warm requests never pay.

| metric | min µs | p50 µs | p99 µs | max µs |
| --------- | ------ | ------ | ------ | ------ |
| hmac      |   4.00 |   5.00 |  13.00 |  17.00 |
| insert    |  76.00 | 107.00 | 2707.00 | 4146.00 |
| update    |  43.00 |  62.00 | 179.00 | 2584.00 |

The medians agree with run 3. The **insert tail does not** — p99 2707 µs against run 3's 263 µs.

### Investigating the insert tail

**Hypothesis:** WAL auto-checkpoints. By default SQLite copies the WAL back into the main
database once it passes 1000 pages, synchronously, on whichever commit crosses the threshold —
a classic source of periodic latency spikes.

**Test:** 300 insert+update pairs, WAL + `synchronous=FULL`, with `wal_autocheckpoint` at its
default (1000) and disabled (0), three repeats each. Inserts slower than 1 ms were logged by
position.

| run | autocheckpoint | p50 µs | p99 µs | max µs | inserts > 1 ms |
| --- | --- | ---: | ---: | ---: | --- |
| 1 | on  | 109 |  216 | 3015 | #299 |
| 1 | off | 106 | 2872 | 3091 | #6, 9, 21, 36, 37, 39, 54, 62, 73, 74, 122, 146, 148, 164, 222, 238, 275, 298 |
| 2 | on  |  76 |   96 |  678 | none |
| 2 | off | 104 |  132 |  246 | none |
| 3 | on  |  76 |   98 |  372 | none |
| 3 | off | 101 |  115 |  285 | none |

**Hypothesis rejected**, three ways over:

- disabling checkpoints did **not** remove the spikes — run 1 had *more* with them off;
- the slow inserts are at **irregular** positions, where checkpointing would be periodic;
- runs 2 and 3 had **no tail at all** in either configuration.

**Conclusion:** the tail is fsync latency variance from the disk and the operating system —
intermittent, irregular, and outside the program's control. It came and went between runs
minutes apart on the same machine. That is the concrete argument against a synchronous disk
write on a latency-critical path: its p50 is fine, but it attaches an uncontrolled tail to
every order that goes through it.

---

## What remains, and what this does not say
- Runs 1–4 were built without `-O2`; run 5 is the first optimised build. The local medians
  barely moved, which is consistent with those costs being system calls and disk I/O rather
  than CPU work the compiler could improve.
- Public testnet REST over consumer Wi-Fi from Seoul. **This says nothing about
  colocated HFT latency**, where the ~52 ms `wait` term is attacked by moving the
  machine, not by changing the code. On a colocated box the ordering inverts and
  the ~110 µs insert becomes the dominant cost of placing an order — which is
  exactly why local and network are reported as separate blocks.
- One machine, one time of day, N=300. p50 is solid, p99 indicative.
- `CFLAGS` is `-Wall` with no `-O2`, so local figures are unoptimised-build figures.
- The insert measured 100 µs in the bench against 74 µs in the isolated experiment;
  the bench interleaves inserts and updates against the same WAL, so they are not
  expected to match exactly.
