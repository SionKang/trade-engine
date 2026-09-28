/*
 * trade.c — an order-execution engine for the Binance Spot testnet, with a toy
 * moving-average strategy on top to drive it.
 *
 * The STRATEGY (the main loop) decides WHEN to trade: it polls the price every few
 * seconds, and when a fast and a slow moving average CROSS it asks for a BUY or SELL.
 *
 * The ENGINE (place_order) decides HOW: three safety gates, then persist the order,
 * sign it, send it, and classify what came back as FILLED, FAILED or UNKNOWN —
 * never claiming more than the exchange's reply actually proves.
 *
 * Build:  make
 * Run:    ./trade              start the bot (Ctrl-C or `touch KILL` to stop)
 *         ./trade --bench N    latency measurement: no credentials, no orders
 * Needs:  BINANCE_API_KEY / BINANCE_API_SECRET in the environment (bot only).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>             // clock_gettime()
#include <math.h>             // round(), fabs()
#include <unistd.h>           // access(), sleep(), getpid(), unlink()
#include <curl/curl.h>        // HTTP(S) requests
#include <openssl/hmac.h>     // HMAC-SHA256 request signing
#include <sqlite3.h>          // local order database
#include <cjson/cJSON.h>      // parse the exchange's JSON replies

/* ----- strategy parameters (tune these) ----- */
#define FAST_N        3        // fast moving-average window (number of samples)
#define SLOW_N        10       // slow moving-average window (number of samples)
#define POLL_SECONDS  2        // how often to poll the price

/* ----- order size and risk limit ----- */
#define ORDER_QTY     0.001    // BTC per order
#define MAX_POSITION  0.1      // hard cap on the bot's net long position, in BTC

/* Testnet only, by design. Every endpoint below is built from this one line. */
#define BASE_URL      "https://testnet.binance.vision"


/* ===== HTTP ============================================================== */
/*
 * libcurl hands back a reply in CHUNKS, calling on_data() for each, so our job is
 * to append them. `userp` is whatever we passed via CURLOPT_WRITEDATA.
 */
struct response { char data[8192]; size_t len; };
static size_t on_data(char *chunk, size_t size, size_t nmemb, void *userp) {
    size_t bytes = size * nmemb;                  // bytes in THIS chunk
    struct response *r = (struct response *)userp;
    if (r->len + bytes >= sizeof(r->data))        // overflow guard: leave room for the '\0'
        return 0;                                 // short count -> libcurl aborts the transfer
    memcpy(r->data + r->len, chunk, bytes);       // copy onto the end
    r->len += bytes; r->data[r->len] = '\0';      // advance, keep it a valid C string
    return bytes;                                 // tell libcurl we consumed it all
}

/*
 * One curl handle for the whole process, reused for every request.
 *
 * A fresh handle per request re-pays DNS, the TCP handshake and the TLS handshake
 * every time — measured at ~48 ms of a ~101 ms median request. A reused handle keeps
 * its connection alive. curl_easy_reset() clears the previous request's options but
 * deliberately KEEPS the live connection, TLS session and DNS cache. The reset is
 * also load-bearing for correctness: without it, POSTFIELDS left over from an order
 * would turn the next price GET into a POST.
 *
 * Single-threaded by design: an easy handle must never be shared across threads.
 */
static CURL *g_curl = NULL;

static CURL *curl_handle(void) {
    if (!g_curl) g_curl = curl_easy_init();
    else         curl_easy_reset(g_curl);
    return g_curl;
}

static void curl_handle_release(void) {
    if (g_curl) { curl_easy_cleanup(g_curl); g_curl = NULL; }
}

/*
 * Where a request's time went. libcurl reports its milestones as CUMULATIVE
 * microseconds from the start (dns <= connect <= tls <= first byte), so adjacent
 * milestones are subtracted to get what each stage actually cost.
 */
struct http_timing { long long dns_ns, tcp_ns, tls_ns, wait_ns, total_ns; };

/*
 * http_get() — GET a URL into `resp`; returns 1 on success, 0 on failure. Pass a
 * struct http_timing to collect the breakdown, or NULL. The order POST does its own
 * setup instead, because it needs a header list and must interrogate the handle.
 */
static int http_get(const char *url, struct response *resp, struct http_timing *t) {
    CURL *h = curl_handle();
    if (!h) return 0;
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, resp);
    // Bounded: a request that never returns is a correctness problem, not a slow one.
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, 2000L);
    curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, 5000L);
    CURLcode rc = curl_easy_perform(h);

    if (t) {
        curl_off_t dns = 0, con = 0, tls = 0, ttfb = 0, tot = 0;
        curl_easy_getinfo(h, CURLINFO_NAMELOOKUP_TIME_T,    &dns);
        curl_easy_getinfo(h, CURLINFO_CONNECT_TIME_T,       &con);
        curl_easy_getinfo(h, CURLINFO_APPCONNECT_TIME_T,    &tls);
        curl_easy_getinfo(h, CURLINFO_STARTTRANSFER_TIME_T, &ttfb);
        curl_easy_getinfo(h, CURLINFO_TOTAL_TIME_T,         &tot);
        // tls is 0 on a reused or plain-HTTP connection, so fall back to connect
        curl_off_t base = (tls > 0) ? tls : con;
        t->dns_ns   = (long long)dns * 1000;
        t->tcp_ns   = (con  > dns)  ? (long long)(con  - dns)  * 1000 : 0;
        t->tls_ns   = (tls  > con)  ? (long long)(tls  - con)  * 1000 : 0;
        t->wait_ns  = (ttfb > base) ? (long long)(ttfb - base) * 1000 : 0;
        t->total_ns = (long long)tot * 1000;
    }
    return rc == CURLE_OK;
}


/* ===== market data ======================================================= */
/* One symbol's quantity rules, read once at startup and then held. */
struct lot_filter { double min_qty, max_qty, step_size; };

/*
 * fetch_symbol_rules() — read a symbol's trading status and LOT_SIZE rule.
 *
 * Called ONCE at startup. Symbol filters change on the order of once a year, and
 * fetching them per order put a whole extra round trip — plus rate-limit weight 20,
 * against the order's own 1 — inside the critical path.
 *
 * Caching is safe because staleness fails safe in BOTH directions: if the filters
 * tighten, the exchange rejects the size and the reply is classified FAILED; if they
 * loosen, we are merely over-restrictive. The exchange stays the authority, so a
 * mid-run filter change needs only a restart.
 *
 * Returns 1 on success, 0 if the symbol is not TRADING or the rules are unreadable.
 */
int fetch_symbol_rules(const char *symbol, struct lot_filter *out) {
    char url[256];
    snprintf(url, sizeof(url), BASE_URL "/api/v3/exchangeInfo?symbol=%s", symbol);

    struct response resp = {0};
    if (!http_get(url, &resp, NULL)) {
        fprintf(stderr, "[rules] could not fetch exchangeInfo for %s\n", symbol);
        return 0;
    }

    cJSON *root = cJSON_Parse(resp.data);
    if (!root) { fprintf(stderr, "[rules] unparseable exchangeInfo\n"); return 0; }

    // dig down: symbols (array) -> [0], the one symbol we asked about
    cJSON *sym0   = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "symbols"), 0);
    cJSON *status = cJSON_GetObjectItem(sym0, "status");

    // We are parsing this anyway, so refuse a halted symbol here rather than
    // discovering it from a rejected order.
    if (!cJSON_IsString(status) || strcmp(status->valuestring, "TRADING") != 0) {
        fprintf(stderr, "[rules] %s status is %s, not TRADING — refusing to start\n",
                symbol, cJSON_IsString(status) ? status->valuestring : "unreadable");
        cJSON_Delete(root);
        return 0;
    }

    int found = 0;
    cJSON *f;
    cJSON_ArrayForEach(f, cJSON_GetObjectItem(sym0, "filters")) {
        cJSON *type = cJSON_GetObjectItem(f, "filterType");
        if (!cJSON_IsString(type) || strcmp(type->valuestring, "LOT_SIZE") != 0) continue;

        // STRINGS in the JSON, deliberately, because they must be exact. Each field
        // is checked before it is dereferenced.
        cJSON *mn = cJSON_GetObjectItem(f, "minQty");
        cJSON *mx = cJSON_GetObjectItem(f, "maxQty");
        cJSON *st = cJSON_GetObjectItem(f, "stepSize");
        if (cJSON_IsString(mn) && cJSON_IsString(mx) && cJSON_IsString(st)) {
            out->min_qty   = strtod(mn->valuestring, NULL);
            out->max_qty   = strtod(mx->valuestring, NULL);
            out->step_size = strtod(st->valuestring, NULL);
            found = 1;
        }
        break;
    }
    cJSON_Delete(root);
    if (!found) fprintf(stderr, "[rules] no usable LOT_SIZE filter for %s\n", symbol);
    return found;
}

/*
 * fetch_price() — GET the current price of a symbol; writes it through *out.
 * Returns 1 on success, 0 on failure. The reply is tiny:
 * {"symbol":"BTCUSDT","price":"60123.45000000"}. This feeds only the strategy's
 * moving averages, which is why a double is acceptable here.
 */
int fetch_price(const char *symbol, double *out) {
    char url[256];
    snprintf(url, sizeof(url), BASE_URL "/api/v3/ticker/price?symbol=%s", symbol);

    struct response resp = {0};
    if (!http_get(url, &resp, NULL)) return 0;

    cJSON *root = cJSON_Parse(resp.data);
    if (!root) return 0;
    cJSON *p = cJSON_GetObjectItem(root, "price");
    int ok = cJSON_IsString(p);
    if (ok) *out = strtod(p->valuestring, NULL);
    cJSON_Delete(root);
    return ok;
}


/* ===== request signing =================================================== */
/*
 * sign() — hex HMAC-SHA256 of the request: its "wax seal".
 *
 * HMAC mixes our secret into the hash, so only a holder of the secret can produce
 * the right output. We send the signature and never the secret; the exchange
 * recomputes it to prove the request is ours and was not altered in transit.
 *
 * HMAC() gives 32 raw bytes and the exchange wants 64 hex chars, so each byte
 * becomes two digits. out_hex must hold >= 65.
 */
void sign(const char *secret, const char *message, char *out_hex) {
    unsigned char raw[32]; unsigned int raw_len = 0;            // SHA-256 = 32 bytes
    HMAC(EVP_sha256(), secret, (int)strlen(secret),
         (const unsigned char *)message, strlen(message), raw, &raw_len);
    for (unsigned int i = 0; i < raw_len; i++)
        sprintf(out_hex + (i*2), "%02x", raw[i]);              // 1 byte -> 2 hex chars
    out_hex[raw_len * 2] = '\0';
}


/* ===== time & ids ======================================================== */
/*
 * now_ms() — WALL-CLOCK milliseconds since 1970, which the exchange's `timestamp`
 * parameter wants. That timestamp is replay protection: the exchange rejects stale
 * requests, so a captured request cannot be replayed forever.
 */
long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/*
 * now_ns() — MONOTONIC nanoseconds, for measuring how long something took.
 *
 * Deliberately not the wall clock: NTP can step that backwards mid-measurement and
 * produce a negative duration, while the monotonic clock only moves forward. Reading
 * it costs tens of nanoseconds, well under 1% of the cheapest thing we time.
 */
static long long now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/*
 * make_client_order_id() — an id like "ord-1782657755991-8988".
 *
 * A NAME, not an idempotency key: it is fresh on every call, so it cannot prevent a
 * duplicate order. What it gives us is a handle for asking the exchange about this
 * order later. Binance documents no length or charset limit, so we stay short and
 * alphanumeric. See "Reliability" in README.md.
 */
void make_client_order_id(char *buf, size_t n) {
    snprintf(buf, n, "ord-%lld-%04d", now_ms(), rand() % 10000);
}


/* ===== database ========================================================== */
/* The statements the bot runs, shared with the benchmark so that what it times is
 * exactly what place_order() executes rather than a lookalike. */
static const char SQL_SCHEMA[] =
    "CREATE TABLE IF NOT EXISTS orders ("
    " client_order_id TEXT PRIMARY KEY, exchange_order_id TEXT,"
    " symbol TEXT, side TEXT, type TEXT, quantity REAL, status TEXT,"
    " created_at_ms INTEGER, updated_at_ms INTEGER);";

static const char SQL_INSERT[] =
    "INSERT INTO orders (client_order_id, symbol, side, type, quantity, status,"
    " created_at_ms, updated_at_ms) VALUES (?,?,?,?,?,?,?,?);";

static const char SQL_UPDATE[] =
    "UPDATE orders SET exchange_order_id = ?, status = ?, updated_at_ms = ? "
    "WHERE client_order_id = ?;";

/*
 * db_set_pragmas() — WAL mode with full syncing.
 *
 * WAL commits by appending to one log and syncing it once, where the default
 * rollback journal writes a journal, syncs, writes the database and syncs again.
 * Same durability promise, half the syncs: 337 us -> 74 us per insert when measured.
 * It also stops readers blocking the writer, so querying trading.db with the sqlite3
 * CLI while the bot trades cannot stall an order write.
 *
 * `PRAGMA journal_mode` RETURNS the mode actually in force, so we read the answer
 * rather than assume it: WAL needs a local filesystem and can legitimately fail.
 * synchronous is per-connection and its default varies by build, so it is set
 * explicitly. NOT synchronous=NORMAL: ~5x faster again, but an OS crash or power
 * loss can then lose recent commits — the exact thing writing first exists to prevent.
 */
static int db_set_pragmas(sqlite3 *db) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "PRAGMA journal_mode=WAL;", -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "[db] journal_mode pragma failed: %s\n", sqlite3_errmsg(db));
        return 0;
    }
    char mode[16] = "?";
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *m = sqlite3_column_text(st, 0);
        if (m) snprintf(mode, sizeof(mode), "%s", (const char *)m);   // copy before finalize
    }
    sqlite3_finalize(st);

    if (sqlite3_exec(db, "PRAGMA synchronous=FULL;", NULL, NULL, NULL) != SQLITE_OK) {
        fprintf(stderr, "[db] synchronous pragma failed: %s\n", sqlite3_errmsg(db));
        return 0;
    }
    if (strcmp(mode, "wal") != 0)
        fprintf(stderr, "[db] warning: journal_mode is \"%s\", not wal — writes will be slower\n",
                mode);
    return 1;
}

/*
 * db_open() — open a database, apply the pragmas and create the schema. Returns the
 * handle, or NULL (already closed) on any failure. The bot and the benchmark both
 * open through here, so they are guaranteed to run under identical settings.
 */
static sqlite3 *db_open(const char *path) {
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        fprintf(stderr, "[db] cannot open %s: %s\n", path, sqlite3_errmsg(db));
        sqlite3_close(db);                        // required even when open fails
        return NULL;
    }
    char *err = NULL;
    if (!db_set_pragmas(db) || sqlite3_exec(db, SQL_SCHEMA, NULL, NULL, &err) != SQLITE_OK) {
        if (err) fprintf(stderr, "[db] cannot create schema: %s\n", err);
        sqlite3_free(err);
        sqlite3_close(db);
        return NULL;
    }
    return db;
}

/*
 * current_position() — the bot's net long position: filled BUYs minus filled SELLs.
 *
 * Returns 1 and writes *out on success, 0 on failure. Failure must mean "unknown",
 * never zero: treating a failed query as a flat position would let the cap wave a
 * BUY through, i.e. the risk check would fail open.
 */
int current_position(sqlite3 *db, double *out) {
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT COALESCE(SUM(CASE side "
            "                      WHEN 'BUY'  THEN  quantity "
            "                      WHEN 'SELL' THEN -quantity "
            "                      ELSE 0 END), 0) "
            "FROM orders WHERE status = 'FILLED';", -1, &q, NULL) != SQLITE_OK) {
        fprintf(stderr, "[db] position query failed: %s\n", sqlite3_errmsg(db));
        return 0;
    }
    int ok = (sqlite3_step(q) == SQLITE_ROW);
    if (ok) *out = sqlite3_column_double(q, 0);
    sqlite3_finalize(q);
    return ok;
}


/* ===== safety gates ====================================================== */
/*
 * kill_switch_engaged() — emergency stop. Returns 1 if a file named "KILL" exists,
 * so an operator can `touch KILL` to halt all order flow instantly: no restart, no
 * redeploy. (F_OK just asks "does this file exist?")
 */
int kill_switch_engaged(void) {
    return access("KILL", F_OK) == 0;
}

/*
 * quantity_ok() — check an order quantity against the LOT_SIZE rule.
 * Returns 1 if valid; otherwise prints why and returns 0.
 */
int quantity_ok(double qty, const struct lot_filter *lot) {
    double minQty = lot->min_qty, maxQty = lot->max_qty, stepSize = lot->step_size;
    if (qty < minQty) { fprintf(stderr, "[reject] qty %.8f < minQty %.8f\n", qty, minQty); return 0; }
    if (qty > maxQty) { fprintf(stderr, "[reject] qty %.8f > maxQty %.8f\n", qty, maxQty); return 0; }

    // Step rule, with two caveats. Binance documents it as `quantity % stepSize == 0`
    // with no minQty offset; this agrees only because BTCUSDT has minQty == stepSize.
    // And exact equality is impossible in floating point (0.00001 is not representable
    // in binary), hence the tolerance — which is why fixed-point integers are the fix.
    double steps = (qty - minQty) / stepSize;
    if (fabs(steps - round(steps)) > 1e-6) {
        fprintf(stderr, "[reject] qty %.8f not a multiple of step %.8f\n", qty, stepSize);
        return 0;
    }
    return 1;
}


/* ===== execution engine ================================================== */
/*
 * place_order() — place ONE market order, safely. The engine the strategy calls.
 *
 * One call runs all three gates, persists the order, signs and sends it, then
 * records what came of it. Safety lives in the engine, not in the caller, so any
 * strategy gets it. The caller owns `db`; we never open or close it.
 *
 * Returns 1 if the request was transmitted, 0 if a gate blocked it or nothing was
 * sent. A 1 does NOT mean the order filled — the row's status says that.
 */
int place_order(sqlite3 *db, const char *api_key, const char *secret,
                const char *symbol, const char *side, double quantity,
                const struct lot_filter *lot) {
    long long t0 = now_ns();                   // decision handed to the engine

    /* GATE 1 — kill-switch */
    if (kill_switch_engaged()) {
        fprintf(stderr, "[KILL] kill-switch engaged — refusing to trade\n");
        return 0;
    }

    /* GATE 2 — exchange-rule validation. Entirely local: the filters were fetched
     * once at startup, so this gate costs no round trip, and a blip on a read-only
     * market-data endpoint cannot stop an order. */
    if (!quantity_ok(quantity, lot))
        return 0;                              // rejected locally — nothing sent

    /* GATE 3 — position cap. It only checks BUYs: a SELL is sent without checking whether
     * the bot actually holds that much BTC. The exchange only rejects it if the whole
     * account is short, so a SELL can go through using BTC the bot never bought. */
    if (strcmp(side, "BUY") == 0) {
        double position;
        if (!current_position(db, &position)) {
            fprintf(stderr, "[reject] position unknown — refusing to trade\n");
            return 0;
        }
        if (position + quantity > MAX_POSITION) {
            fprintf(stderr, "[reject] position cap: have %.8f, +%.8f would exceed %.8f\n",
                    position, quantity, MAX_POSITION);
            return 0;
        }
    }

    /* (1) a name for this order */
    char client_id[40];
    make_client_order_id(client_id, sizeof(client_id));

    /* (2) record the order PENDING *before* sending it, so a crash cannot lose it.
     * No row, no send: if we cannot record the order we must not place it. (A
     * duplicate client_order_id would also fail here, via the PRIMARY KEY — but the
     * id is fresh on every call, so this does not guard against double-sending.) */
    sqlite3_stmt *stmt = NULL;
    int ins_rc = SQLITE_ERROR;
    if (sqlite3_prepare_v2(db, SQL_INSERT, -1, &stmt, NULL) == SQLITE_OK) {
        long long now = now_ms();
        sqlite3_bind_text  (stmt, 1, client_id, -1, SQLITE_STATIC);   // 1-indexed
        sqlite3_bind_text  (stmt, 2, symbol,    -1, SQLITE_STATIC);
        sqlite3_bind_text  (stmt, 3, side,      -1, SQLITE_STATIC);
        sqlite3_bind_text  (stmt, 4, "MARKET",  -1, SQLITE_STATIC);
        sqlite3_bind_double(stmt, 5, quantity);
        sqlite3_bind_text  (stmt, 6, "PENDING", -1, SQLITE_STATIC);
        sqlite3_bind_int64 (stmt, 7, now);
        sqlite3_bind_int64 (stmt, 8, now);
        ins_rc = sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
    if (ins_rc != SQLITE_DONE) {
        fprintf(stderr, "[db] insert failed (%s) — not sending %s\n", sqlite3_errmsg(db), client_id);
        return 0;
    }
    printf("[db] PENDING %s %s %.8f (%s)\n", side, symbol, quantity, client_id);

    /* (3) build the request body, sign it, and POST it. */
    char params[700];
    snprintf(params, sizeof(params),
        "symbol=%s&side=%s&type=MARKET&quantity=%.8f&newClientOrderId=%s&timestamp=%lld",
        symbol, side, quantity, client_id, now_ms());
    char sig[65];
    sign(secret, params, sig);
    char body[900];
    snprintf(body, sizeof(body), "%s&signature=%s", params, sig);

    CURL *handle = curl_handle();
    char api_header[256];
    snprintf(api_header, sizeof(api_header), "X-MBX-APIKEY: %s", api_key);   // "who I am"
    struct curl_slist *headers = curl_slist_append(NULL, api_header);

    struct response resp = {0};
    curl_easy_setopt(handle, CURLOPT_URL, BASE_URL "/api/v3/order");
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(handle, CURLOPT_POSTFIELDS, body);        // a body makes this a POST
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &resp);
    // Bounded, so a stalled connection cannot hang the bot. A timeout lands in the
    // UNKNOWN branch below, which is exactly the honest outcome.
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS, 2000L);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, 5000L);

    long long t_start = now_ns();                              // local work done; request goes out
    CURLcode rc = curl_easy_perform(handle);                   // sends, then waits for the WHOLE reply
    long long t_reply = now_ns();                              // reply fully received

    /* Read these now, before the next request resets the handle.
     *
     * CURLINFO_RESPONSE_CODE is the HTTP status, needed because a 5XX body parses as
     * perfectly valid JSON — it just is not an order.
     *
     * CURLINFO_PRETRANSFER_TIME stays zero until libcurl is about to put our bytes on
     * the wire, so zero PROVES DNS / TCP / TLS failed and the matching engine cannot
     * have seen this order. It is the only evidence we have that separates "never
     * sent" from "sent, no answer". */
    long http_code = 0;
    curl_off_t pretransfer_us = 0;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_getinfo(handle, CURLINFO_PRETRANSFER_TIME_T, &pretransfer_us);
    curl_slist_free_all(headers);          // the header list is per-request; the handle is not

    /* (4) CLASSIFY the outcome, then transition the row. Two rules decide it all:
     *   write FILLED only when the exchange said FILLED;
     *   write FAILED only when something proves no order exists;
     *   everything else is UNKNOWN — the default, not the exception.
     * Claiming FAILED where the truth is UNKNOWN is the dangerous direction: the
     * database would contradict reality for an order that actually filled, and the
     * position cap would under-count real exposure. */
    const char *new_status = "UNKNOWN";
    char exch_id[32] = "";                         // stays "" if there is no orderId
    cJSON *root = NULL;

    if (rc != CURLE_OK) {
        if (pretransfer_us == 0) {
            new_status = "FAILED";                 // nothing left this machine
            fprintf(stderr, "[send] %s: not transmitted (%s) — no order exists\n",
                    client_id, curl_easy_strerror(rc));
        } else {
            fprintf(stderr, "[send] %s: failed after transmitting (%s) — outcome UNKNOWN\n",
                    client_id, curl_easy_strerror(rc));
        }
    } else {
        root = cJSON_Parse(resp.data);             // NULL for an empty or malformed body
        cJSON *status_item = root ? cJSON_GetObjectItem(root, "status") : NULL;
        cJSON *code_item   = root ? cJSON_GetObjectItem(root, "code")   : NULL;
        long long err_code = cJSON_IsNumber(code_item) ? (long long)code_item->valuedouble : 0;

        if (http_code == 200 && cJSON_IsString(status_item)) {
            // A real order object. Keep the exchange's id either way: it is the
            // handle for querying this order later.
            cJSON *oid = cJSON_GetObjectItem(root, "orderId");
            if (cJSON_IsNumber(oid))
                snprintf(exch_id, sizeof(exch_id), "%lld", (long long)oid->valuedouble);

            if (strcmp(status_item->valuestring, "FILLED") == 0) {
                new_status = "FILLED";
            } else {
                // NEW, PARTIALLY_FILLED, EXPIRED... an order exists but its outcome
                // is not settled, and this project has no state for "resting order".
                fprintf(stderr, "[reply] %s: exchange says %s, not FILLED — recording UNKNOWN\n",
                        client_id, status_item->valuestring);
            }
        } else if (err_code == -1007) {
            // Binance: "Timeout waiting for response from backend server. Send status
            // unknown; execution status unknown." The order may be live.
            fprintf(stderr, "[reply] %s: -1007 TIMEOUT inside Binance — outcome UNKNOWN\n",
                    client_id);
        } else if (http_code >= 400 && http_code < 500 && err_code != 0) {
            // A parsed Binance error code on a 4XX: the API refused the request (bad
            // timestamp, bad signature, filter violation, rate limit), so no order was
            // created. This is the only reply that proves refusal.
            new_status = "FAILED";
            cJSON *msg = cJSON_GetObjectItem(root, "msg");
            fprintf(stderr, "[reply] %s: refused, HTTP %ld code %lld: %s\n", client_id,
                    http_code, err_code, cJSON_IsString(msg) ? msg->valuestring : "(no msg)");
        } else {
            // 5XX, an empty body, an unparseable body, or a 200 that is not an order:
            // the request may well have reached the matching engine.
            fprintf(stderr, "[reply] %s: HTTP %ld, unusable reply — outcome UNKNOWN\n",
                    client_id, http_code);
        }
    }

    sqlite3_stmt *u = NULL;
    int upd_rc = SQLITE_ERROR;
    if (sqlite3_prepare_v2(db, SQL_UPDATE, -1, &u, NULL) == SQLITE_OK) {
        sqlite3_bind_text (u, 1, exch_id,    -1, SQLITE_STATIC);
        sqlite3_bind_text (u, 2, new_status, -1, SQLITE_STATIC);
        sqlite3_bind_int64(u, 3, now_ms());
        sqlite3_bind_text (u, 4, client_id,  -1, SQLITE_STATIC);
        upd_rc = sqlite3_step(u);
    }
    sqlite3_finalize(u);
    if (upd_rc == SQLITE_DONE)
        printf("[db] %s %s (exchange_order_id=%s)\n", new_status, client_id, exch_id);
    else   // the order happened; we failed to write down what happened. Say so.
        fprintf(stderr, "[db] could not record %s for %s (%s) — row still PENDING\n",
                new_status, client_id, sqlite3_errmsg(db));
    if (root) cJSON_Delete(root);

    /* Where this order's time went, split by what each part means:
     *   pre-send    — gates, INSERT and signing: our own work before the request leaves.
     *                 The only part of reaching the exchange that this code controls.
     *   round-trip  — the whole HTTP exchange: the request going out, the exchange
     *                 processing it, and the reply coming back. This is NOT the time the
     *                 order took to reach the exchange: that moment falls somewhere inside
     *                 the round trip, and the client has no clock that can see it.
     *   after-reply — parse + UPDATE. Costs this order nothing, but delays the next
     *                 decision, and is how long the database can disagree with reality.
     *   total       — the whole call, i.e. when the strategy can act again.
     * The asymmetry worth knowing: the INSERT is in pre-send, so it delays the order; the
     * UPDATE comes after the reply, so it does not. Only one of our two writes costs
     * latency. */
    long long t_done = now_ns();
    printf("[lat] %s pre-send %.2f ms, round-trip %.2f ms, after-reply %.2f ms, total %.2f ms\n",
           client_id,
           (double)(t_start - t0)      / 1e6,
           (double)(t_reply - t_start) / 1e6,
           (double)(t_done  - t_reply) / 1e6,
           (double)(t_done  - t0)      / 1e6);

    return (rc == CURLE_OK || pretransfer_us != 0) ? 1 : 0;
}


/* ===== strategy ========================================================== */
/* average() — simple mean of `len` doubles starting at a[0]. */
double average(const double *a, int len) {
    double s = 0.0;
    for (int i = 0; i < len; i++) s += a[i];
    return s / len;
}


/* ===== benchmark ========================================================= */
static int cmp_ll(const void *a, const void *b) {
    long long x = *(const long long *)a, y = *(const long long *)b;
    return (x > y) - (x < y);                      // not x-y: that can overflow
}

/*
 * pct() — the p-th percentile of an already-sorted array: the sample at index
 * floor(p * (n - 1) / 100), i.e. the LOWER of the two samples a linearly interpolated
 * percentile would fall between. It always returns a real sample, never an invented
 * in-between value. Percentile definitions differ from one another by up to a sample,
 * so the one used is spelled out here to keep the numbers reproducible.
 */
static long long pct(const long long *v, int n, int p) {
    return v[(p * (n - 1)) / 100];
}

/* Rows print as a markdown table so the output pastes straight into a report.
 * `scale` converts nanoseconds into the header's unit: 1e3 for us, 1e6 for ms. */
static void report_head(const char *title, const char *unit, int n) {
    printf("\n%s (n=%d)\n", title, n);
    if (n < 100)
        printf("note: at n=%d, p99 is just one of the two largest samples and says little.\n", n);
    printf("\n| metric    | min %-2s | p50 %-2s | p99 %-2s | max %-2s |\n", unit, unit, unit, unit);
    printf("| --------- | ------ | ------ | ------ | ------ |\n");
}

static void report_row(const char *name, long long *v, int n, double scale) {
    qsort(v, (size_t)n, sizeof *v, cmp_ll);
    printf("| %-9s | %6.2f | %6.2f | %6.2f | %6.2f |\n", name,
           (double)v[0] / scale,            (double)pct(v, n, 50) / scale,
           (double)pct(v, n, 99) / scale,   (double)v[n - 1] / scale);
}

/*
 * bench() — measure where the time goes, without placing a single order.
 *
 * Reads one public endpoint N times and runs the local work N times in isolation;
 * needs no credentials. Two blocks, because they answer different questions: the
 * network block is latency a laptop on the public internet cannot change, the local
 * block is the software cost this code actually controls.
 *
 * Request 1 is reported on its own as "cold" (it pays DNS, TCP and TLS); requests
 * 2..N reuse that connection and form the distribution.
 */
static int bench(int n) {
    if (n < 2)    n = 2;
    if (n > 2000) n = 2000;         // ticker/price is weight 1, the IP limit 6000/min

    /* One allocation, sliced into eight sample arrays. */
    long long *buf = malloc((size_t)n * 8 * sizeof *buf);
    if (!buf) { fprintf(stderr, "bench: out of memory\n"); return 1; }
    long long *t_dns  = buf,       *t_tcp = buf + n,     *t_tls = buf + 2*n,
              *t_wait = buf + 3*n, *t_tot = buf + 4*n,
              *t_hmac = buf + 5*n, *t_ins = buf + 6*n,   *t_upd = buf + 7*n;

    printf("bench: %d samples, no orders, no credentials\n", n);

    /* ---------- network: repeated public GETs ---------- */
    struct http_timing first = {0, 0, 0, 0, 0};
    int got = 0, failed = 0;
    for (int i = 0; i < n; i++) {
        struct response resp = {0};
        struct http_timing t = {0, 0, 0, 0, 0};
        if (!http_get(BASE_URL "/api/v3/ticker/price?symbol=BTCUSDT", &resp, &t)) {
            failed++; continue;
        }
        if (i == 0) { first = t; continue; }       // reported on its own, below
        t_dns[got]  = t.dns_ns;  t_tcp[got] = t.tcp_ns;   t_tls[got] = t.tls_ns;
        t_wait[got] = t.wait_ns; t_tot[got] = t.total_ns; got++;
    }
    if (failed) printf("  (%d requests failed and were excluded)\n", failed);

    printf("\nfirst request, cold: dns %.2f  tcp %.2f  tls %.2f  wait %.2f  total %.2f ms\n",
           (double)first.dns_ns / 1e6, (double)first.tcp_ns / 1e6, (double)first.tls_ns / 1e6,
           (double)first.wait_ns / 1e6, (double)first.total_ns / 1e6);

    if (got > 0) {
        report_head("network, requests 2..N", "ms", got);
        report_row("dns",   t_dns,  got, 1e6);
        report_row("tcp",   t_tcp,  got, 1e6);
        report_row("tls",   t_tls,  got, 1e6);
        report_row("wait",  t_wait, got, 1e6);    // request sent -> first byte back
        report_row("total", t_tot,  got, 1e6);
    }

    /* ---------- local: the work this program performs per order ---------- */
    /* A representative request body. The key is a placeholder, never a credential:
     * HMAC cost depends on message length, not on key value. */
    char params[700];
    snprintf(params, sizeof(params),
        "symbol=BTCUSDT&side=BUY&type=MARKET&quantity=%.8f"
        "&newClientOrderId=ord-%lld-0000&timestamp=%lld", ORDER_QTY, now_ms(), now_ms());
    char sig[65];
    /* One discarded warm-up: the first HMAC pays OpenSSL's one-time setup (~16 ms
     * against ~5 us steady state), which would otherwise set both max and p99. */
    sign("bench-placeholder-not-a-real-secret", params, sig);
    for (int i = 0; i < n; i++) {
        long long a = now_ns();
        sign("bench-placeholder-not-a-real-secret", params, sig);
        t_hmac[i] = now_ns() - a;
    }

    /* A real file on the same filesystem as trading.db: the cost we want is the
     * durable write, and :memory: would measure nothing. Per-pid name, removed after. */
    char db_path[64];
    snprintf(db_path, sizeof(db_path), "trade_bench_%d.db", (int)getpid());
    int rows = 0;
    sqlite3 *bdb = db_open(db_path);
    if (bdb) {
        for (int i = 0; i < n; i++) {
            char id[40];
            snprintf(id, sizeof(id), "bench-%d", i);
            sqlite3_stmt *st = NULL;

            long long a = now_ns();                // same statement place_order() runs
            if (sqlite3_prepare_v2(bdb, SQL_INSERT, -1, &st, NULL) != SQLITE_OK) break;
            sqlite3_bind_text  (st, 1, id,        -1, SQLITE_STATIC);
            sqlite3_bind_text  (st, 2, "BTCUSDT", -1, SQLITE_STATIC);
            sqlite3_bind_text  (st, 3, "BUY",     -1, SQLITE_STATIC);
            sqlite3_bind_text  (st, 4, "MARKET",  -1, SQLITE_STATIC);
            sqlite3_bind_double(st, 5, ORDER_QTY);
            sqlite3_bind_text  (st, 6, "PENDING", -1, SQLITE_STATIC);
            sqlite3_bind_int64 (st, 7, now_ms());
            sqlite3_bind_int64 (st, 8, now_ms());
            int step_rc = sqlite3_step(st);
            sqlite3_finalize(st);
            t_ins[rows] = now_ns() - a;
            if (step_rc != SQLITE_DONE) break;

            a = now_ns();
            if (sqlite3_prepare_v2(bdb, SQL_UPDATE, -1, &st, NULL) != SQLITE_OK) break;
            sqlite3_bind_text (st, 1, "999",    -1, SQLITE_STATIC);
            sqlite3_bind_text (st, 2, "FILLED", -1, SQLITE_STATIC);
            sqlite3_bind_int64(st, 3, now_ms());
            sqlite3_bind_text (st, 4, id,       -1, SQLITE_STATIC);
            sqlite3_step(st);
            sqlite3_finalize(st);
            t_upd[rows] = now_ns() - a;
            rows++;
        }
        sqlite3_close(bdb);
    }
    /* WAL adds -wal and -shm sidecars beside the database; remove all three. */
    char side_file[80];
    unlink(db_path);
    snprintf(side_file, sizeof(side_file), "%s-wal", db_path); unlink(side_file);
    snprintf(side_file, sizeof(side_file), "%s-shm", db_path); unlink(side_file);

    report_head("local work", "us", n);
    report_row("hmac", t_hmac, n, 1e3);
    if (rows > 0) {
        report_row("insert", t_ins, rows, 1e3);   // before the send: on the path to market
        report_row("update", t_upd, rows, 1e3);   // after the send: not on that path
    }

    printf("\nrate limit used: %d requests x weight 1 = %d of 6000 per minute\n", n, n);
    printf("note: public testnet REST over the open internet. Says nothing about\n"
           "      colocated latency; the local block is the only part this code owns.\n");

    free(buf);
    return 0;
}


/* ===== the bot =========================================================== */
static int run_bot(void) {
    const char *api_key = getenv("BINANCE_API_KEY");
    const char *secret  = getenv("BINANCE_API_SECRET");
    if (!api_key || !secret) { fprintf(stderr, "Missing API credentials\n"); return 1; }

    sqlite3 *db = db_open("trading.db");
    if (!db) return 1;

    /* Exchange rules, fetched ONCE. Failing here is fatal on purpose: we will not
     * trade a symbol whose size rules we could not read. */
    struct lot_filter lot;
    if (!fetch_symbol_rules("BTCUSDT", &lot)) { sqlite3_close(db); return 1; }
    printf("[rules] BTCUSDT LOT_SIZE min %.8f max %.8f step %.8f (fetched once)\n",
           lot.min_qty, lot.max_qty, lot.step_size);

    srand((unsigned)time(NULL));     // seed once, or rand() repeats every run

    /* ---------- strategy loop ----------
     * Poll price -> update moving averages -> trade on a fast/slow crossover. Every
     * trade still goes through place_order()'s gates. Stop with Ctrl-C, or with
     * `touch KILL`, which both halts trading AND ends the loop. */
    printf("[strategy] live: fast=%d slow=%d poll=%ds qty=%.4f. Ctrl-C or `touch KILL` to stop.\n",
           FAST_N, SLOW_N, POLL_SECONDS, ORDER_QTY);

    double hist[SLOW_N];        // rolling window of recent prices (oldest .. newest)
    int n = 0;                  // samples collected so far (caps at SLOW_N)
    int prev_fast_above = -1;   // previous (fast > slow) state; -1 = not known yet

    while (1) {
        if (kill_switch_engaged()) { printf("[strategy] KILL present — stopping.\n"); break; }

        double price;
        if (!fetch_price("BTCUSDT", &price)) {
            fprintf(stderr, "[strategy] price fetch failed; retrying\n");
            sleep(POLL_SECONDS);
            continue;
        }

        /* push the new price into the rolling window */
        if (n < SLOW_N) {
            hist[n++] = price;                                       // still filling up
        } else {
            memmove(hist, hist + 1, (SLOW_N - 1) * sizeof(double));  // drop the oldest
            hist[SLOW_N - 1] = price;                                // append the newest
        }

        if (n < SLOW_N) {
            printf("[warmup] price %.2f  (%d/%d samples)\n", price, n, SLOW_N);
        } else {
            double fast = average(&hist[SLOW_N - FAST_N], FAST_N);   // mean of last FAST_N
            double slow = average(hist, SLOW_N);                     // mean of all SLOW_N
            int fast_above = fast > slow;
            printf("[tick] price %.2f  fast %.2f  slow %.2f  (%s)\n",
                   price, fast, slow, fast_above ? "fast>slow" : "fast<slow");

            /* a CROSS = the fast/slow relationship flipped since the last tick */
            if (prev_fast_above != -1 && fast_above != prev_fast_above) {
                const char *side = fast_above ? "BUY" : "SELL";
                printf("[signal] %s cross -> %s\n", fast_above ? "bullish" : "bearish", side);
                place_order(db, api_key, secret, "BTCUSDT", side, ORDER_QTY, &lot);
            }
            prev_fast_above = fast_above;
        }

        sleep(POLL_SECONDS);
    }

    sqlite3_close(db);
    return 0;
}


/* ===== main ============================================================== */
int main(int argc, char **argv) {
    /* libcurl's global state, initialised explicitly because one handle lives for
     * the whole process (see curl_handle()), and torn down on the single exit path. */
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        fprintf(stderr, "curl_global_init failed\n");
        return 1;
    }

    int rc = (argc >= 2 && strcmp(argv[1], "--bench") == 0)
           ? bench(argc >= 3 ? atoi(argv[2]) : 100)     // measurement only: no orders
           : run_bot();

    curl_handle_release();
    curl_global_cleanup();
    return rc;
}
