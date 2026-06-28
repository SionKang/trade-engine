/*
 * trade.c — a tiny autonomous trading bot for the Binance testnet.
 *
 * main() runs a STRATEGY LOOP: every few seconds it polls the live price, tracks
 * a fast and a slow moving average, and when they CROSS it calls place_order()
 * to BUY (bullish cross) or SELL (bearish cross).
 *
 * place_order() is the EXECUTION ENGINE: one call runs three safety gates
 * (kill-switch, exchange-rule validation, position cap), then persists + signs +
 * sends the order and transitions the DB row. The strategy decides WHEN to
 * trade; the engine handles HOW, safely.
 *
 * Build:  make
 * Run:    ./trade              -> start the bot (Ctrl-C or `touch KILL` to stop)
 * Needs:  BINANCE_API_KEY / BINANCE_API_SECRET exported in the shell.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <math.h>             // round(), fabs()
#include <unistd.h>           // access(), sleep()
#include <curl/curl.h>        // HTTP(S) requests
#include <openssl/hmac.h>     // HMAC-SHA256 request signing
#include <sqlite3.h>          // local order database
#include <cjson/cJSON.h>      // parse the exchange's JSON replies

/* ----- strategy parameters (tune these) ----- */
#define FAST_N        3        // fast moving-average window (number of samples)
#define SLOW_N        10       // slow moving-average window (number of samples)
#define POLL_SECONDS  2        // how often to poll the price
#define ORDER_QTY     0.001    // size of each order


/* ===== response capture ================================================== */
/*
 * libcurl does not hand back the whole reply at once — as bytes arrive off the
 * network it repeatedly calls on_data() with a CHUNK. Our job: append each chunk
 * onto resp.data. (The "callback" pattern: we give libcurl a function and it
 * calls us back.) `userp` is whatever we passed via CURLOPT_WRITEDATA.
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


/* ===== market data ====================================================== */
/*
 * fetch_lot_size() — read a symbol's LOT_SIZE rule from the exchange.
 *
 * GETs exchangeInfo and digs out the quantity rules so we can validate an order
 * BEFORE sending it. Writes minQty/maxQty/stepSize back THROUGH the pointers (a
 * C way to "return" several values at once). Returns 1 on success, 0 on failure.
 */
int fetch_lot_size(const char *symbol, double *minQty, double *maxQty, double *stepSize) {
    char url[256];
    snprintf(url, sizeof(url),
        "https://testnet.binance.vision/api/v3/exchangeInfo?symbol=%s", symbol);

    // GET = same libcurl setup as our order, but WITHOUT POSTFIELDS,
    // so libcurl defaults to GET (we're reading, not acting).
    CURL *h = curl_easy_init();
    struct response resp = {0};
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &resp);
    CURLcode rc = curl_easy_perform(h);
    curl_easy_cleanup(h);
    if (rc != CURLE_OK) return 0;

    cJSON *root = cJSON_Parse(resp.data);
    if (!root) return 0;

    // dig down: symbols (array) -> [0] -> filters (array)
    cJSON *symbols = cJSON_GetObjectItem(root, "symbols");
    cJSON *sym0    = cJSON_GetArrayItem(symbols, 0);        // first (only) symbol
    cJSON *filters = cJSON_GetObjectItem(sym0, "filters");

    int found = 0;
    cJSON *f;
    cJSON_ArrayForEach(f, filters) {                       // loop over the filters array
        cJSON *type = cJSON_GetObjectItem(f, "filterType");
        if (cJSON_IsString(type) && strcmp(type->valuestring, "LOT_SIZE") == 0) {
            // these values are STRINGS in the JSON (like price), so strtod them
            *minQty   = strtod(cJSON_GetObjectItem(f, "minQty")->valuestring,   NULL);
            *maxQty   = strtod(cJSON_GetObjectItem(f, "maxQty")->valuestring,   NULL);
            *stepSize = strtod(cJSON_GetObjectItem(f, "stepSize")->valuestring, NULL);
            found = 1;
            break;
        }
    }
    cJSON_Delete(root);
    return found;
}

/*
 * fetch_price() — GET the current price of a symbol; writes it through *out.
 * Returns 1 on success, 0 on failure. Same GET+parse shape as fetch_lot_size,
 * just a tiny reply: {"symbol":"BTCUSDT","price":"60123.45000000"}.
 * (price is a STRING in the JSON, so strtod it.)
 */
int fetch_price(const char *symbol, double *out) {
    char url[256];
    snprintf(url, sizeof(url),
        "https://testnet.binance.vision/api/v3/ticker/price?symbol=%s", symbol);
    CURL *h = curl_easy_init();
    struct response resp = {0};
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &resp);
    CURLcode rc = curl_easy_perform(h);
    curl_easy_cleanup(h);
    if (rc != CURLE_OK) return 0;

    cJSON *root = cJSON_Parse(resp.data);
    if (!root) return 0;
    cJSON *p = cJSON_GetObjectItem(root, "price");
    int ok = cJSON_IsString(p);
    if (ok) *out = strtod(p->valuestring, NULL);
    cJSON_Delete(root);
    return ok;
}


/* ===== request signing ================================================== */
/*
 * sign() — produce a hex HMAC-SHA256 signature: the request's "wax seal".
 *
 * HMAC mixes our SECRET key into the hash, so only someone holding the secret
 * can produce the correct output. We send the signature but NEVER the secret;
 * the exchange (which also has our secret) recomputes it and checks they match,
 * proving the request is really us and was not tampered with in transit.
 *
 * HMAC() outputs 32 RAW bytes; the exchange wants them as a 64-char HEX string,
 * so the loop converts each byte to 2 hex digits. out_hex must hold >= 65.
 */
void sign(const char *secret, const char *message, char *out_hex) {
    unsigned char raw[32]; unsigned int raw_len = 0;            // SHA-256 = 32 bytes
    HMAC(EVP_sha256(), secret, (int)strlen(secret),
         (const unsigned char *)message, strlen(message), raw, &raw_len);
    for (unsigned int i = 0; i < raw_len; i++)
        sprintf(out_hex + (i*2), "%02x", raw[i]);              // 1 byte -> 2 hex chars
    out_hex[raw_len * 2] = '\0';
}


/* ===== time & id helpers ================================================ */
/*
 * now_ms() — current time in milliseconds since 1970, which the exchange's
 * `timestamp` parameter wants. The timestamp is replay protection: the exchange
 * rejects requests whose timestamp is too old, so a captured request can't be
 * resent forever. (13 digits in our era; if you ever see 10, you used seconds.)
 */
long long now_ms(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/*
 * make_client_order_id() — generate a unique id like "ord-1782657755991-8988".
 *
 * This is our idempotency key: minted once per order. The ms timestamp makes it
 * near-unique; the random suffix breaks ties within the same millisecond.
 * (Binance caps clientOrderId at 36 chars; we're comfortably under.)
 */
void make_client_order_id(char *buf, size_t n) {
    snprintf(buf, n, "ord-%lld-%04d", now_ms(), rand() % 10000);
}


/* ===== safety gates ===================================================== */
/*
 * kill_switch_engaged() — emergency stop. Returns 1 if a file named "KILL"
 * exists. Ops can `touch KILL` to halt all order flow instantly — no restart,
 * no redeploy. (F_OK just asks "does this file exist?")
 */
int kill_switch_engaged(void) {
    return access("KILL", F_OK) == 0;
}

/*
 * quantity_ok() — check an order quantity against a LOT_SIZE rule.
 * Returns 1 if valid; otherwise prints why and returns 0.
 */
int quantity_ok(double qty, double minQty, double maxQty, double stepSize) {
    if (qty < minQty) { fprintf(stderr, "[reject] qty %.8f < minQty %.8f\n", qty, minQty); return 0; }
    if (qty > maxQty) { fprintf(stderr, "[reject] qty %.8f > maxQty %.8f\n", qty, maxQty); return 0; }

    // Step rule: (qty - minQty) must be a whole number of steps.
    // You CANNOT use exact equality here — 0.00001 isn't exactly representable
    // in binary floating point, so the division won't land on a clean integer.
    // Instead, check the result is within a tiny tolerance of a whole number.
    // (This float trap is exactly why exchanges ship sizes/prices as strings.)
    double steps = (qty - minQty) / stepSize;
    if (fabs(steps - round(steps)) > 1e-6) {
        fprintf(stderr, "[reject] qty %.8f not a multiple of step %.8f\n", qty, stepSize);
        return 0;
    }
    return 1;
}

/*
 * current_position() — our net long position = sum of quantity over FILLED BUYs.
 * Reads an aggregate from the DB. SUM adds the column across matching rows;
 * COALESCE(.,0) turns a no-rows NULL into 0 so we always get a clean number.
 */
double current_position(sqlite3 *db) {
    double pos = 0.0;
    sqlite3_stmt *q;
    sqlite3_prepare_v2(db,
        "SELECT COALESCE(SUM(quantity), 0) FROM orders "
        "WHERE status = 'FILLED' AND side = 'BUY';", -1, &q, NULL);
    if (sqlite3_step(q) == SQLITE_ROW)
        pos = sqlite3_column_double(q, 0);     // read column 0 as a number
    sqlite3_finalize(q);
    return pos;
}


/* ===== execution engine ================================================= */
/*
 * place_order() — safely place ONE market order. This is the engine the strategy
 * calls. One call runs ALL THREE safety gates, then persists + signs + sends the
 * order and transitions the DB row. Parameterized by symbol/side/quantity, so
 * the caller gets the safe path no matter what — safety is baked into the ENGINE,
 * not the caller. The caller owns `db` (we never open or close it here).
 *
 * Returns 1 if the order was sent to the exchange; 0 if a gate blocked it or an
 * error occurred before sending.
 */
int place_order(sqlite3 *db, const char *api_key, const char *secret,
                const char *symbol, const char *side, double quantity) {
    /* GATE 1 — kill-switch */
    if (kill_switch_engaged()) {
        fprintf(stderr, "[KILL] kill-switch engaged — refusing to trade\n");
        return 0;
    }

    /* GATE 2 — exchange-rule validation */
    double minQty, maxQty, stepSize;
    if (!fetch_lot_size(symbol, &minQty, &maxQty, &stepSize)) {
        fprintf(stderr, "couldn't fetch exchange rules — refusing to trade\n");
        return 0;
    }
    if (!quantity_ok(quantity, minQty, maxQty, stepSize))
        return 0;                                  // rejected locally — nothing sent

    /* GATE 3 — position cap (BUYs only: a SELL reduces exposure, never grows it) */
    if (strcmp(side, "BUY") == 0) {
        const double MAX_POSITION = 0.1;           // max BTC we'll ever hold
        double position = current_position(db);
        if (position + quantity > MAX_POSITION) {
            fprintf(stderr, "[reject] position cap: have %.8f, +%.8f would exceed %.8f\n",
                    position, quantity, MAX_POSITION);
            return 0;
        }
    }

    /* (1) fresh idempotency key for this order */
    char client_id[40];
    make_client_order_id(client_id, sizeof(client_id));

    /* (2) record the order PENDING *before* sending it (so a crash can't lose it). */
    const char *ins =
        "INSERT INTO orders (client_order_id, symbol, side, type, quantity, status,"
        " created_at_ms, updated_at_ms) VALUES (?,?,?,?,?,?,?,?);";
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, ins, -1, &stmt, NULL);
    long long now = now_ms();
    sqlite3_bind_text  (stmt, 1, client_id, -1, SQLITE_STATIC);   // placeholders are 1-indexed
    sqlite3_bind_text  (stmt, 2, symbol,    -1, SQLITE_STATIC);
    sqlite3_bind_text  (stmt, 3, side,      -1, SQLITE_STATIC);
    sqlite3_bind_text  (stmt, 4, "MARKET",  -1, SQLITE_STATIC);
    sqlite3_bind_double(stmt, 5, quantity);
    sqlite3_bind_text  (stmt, 6, "PENDING", -1, SQLITE_STATIC);
    sqlite3_bind_int64 (stmt, 7, now);
    sqlite3_bind_int64 (stmt, 8, now);
    int ins_rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    /* IDEMPOTENCY GUARD: a duplicate client_order_id violates the PRIMARY KEY, so
     * the DB refuses it — and we refuse to send. The structural guarantee that we
     * can never double-send. */
    if (ins_rc == SQLITE_CONSTRAINT) {
        printf("[guard] %s already recorded — not sending again\n", client_id);
        return 0;
    } else if (ins_rc != SQLITE_DONE) {
        fprintf(stderr, "insert failed: %s\n", sqlite3_errmsg(db));
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

    CURL *handle = curl_easy_init();
    char api_header[256];
    snprintf(api_header, sizeof(api_header), "X-MBX-APIKEY: %s", api_key);   // "who I am" header
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, api_header);

    struct response resp = {0};
    curl_easy_setopt(handle, CURLOPT_URL, "https://testnet.binance.vision/api/v3/order");
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(handle, CURLOPT_POSTFIELDS, body);   // a body makes this a POST
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &resp);
    CURLcode rc = curl_easy_perform(handle);              // the request actually fires here
    curl_slist_free_all(headers);
    curl_easy_cleanup(handle);

    if (rc != CURLE_OK) {
        fprintf(stderr, "send failed: %s\n", curl_easy_strerror(rc));
        return 0;
    }

    /* (4) parse the reply and transition the row. status is a STRING; orderId a NUMBER. */
    cJSON *root = cJSON_Parse(resp.data);
    if (!root) { fprintf(stderr, "couldn't parse exchange reply\n"); return 0; }

    cJSON *status_item = cJSON_GetObjectItem(root, "status");
    const char *new_status;
    char exch_id[32] = "";                         // stays "" if there is no orderId
    if (cJSON_IsString(status_item)) {
        new_status = status_item->valuestring;     // FILLED, NEW, ...
        cJSON *oid = cJSON_GetObjectItem(root, "orderId");
        if (cJSON_IsNumber(oid))
            snprintf(exch_id, sizeof(exch_id), "%lld", (long long)oid->valuedouble);
    } else {
        new_status = "REJECTED";                   // error reply: {"code":..,"msg":..}
        cJSON *msg = cJSON_GetObjectItem(root, "msg");
        fprintf(stderr, "order rejected: %s\n", cJSON_IsString(msg) ? msg->valuestring : "unknown");
    }

    const char *upd =
        "UPDATE orders SET exchange_order_id = ?, status = ?, updated_at_ms = ? "
        "WHERE client_order_id = ?;";
    sqlite3_stmt *u;
    sqlite3_prepare_v2(db, upd, -1, &u, NULL);
    sqlite3_bind_text (u, 1, exch_id,    -1, SQLITE_STATIC);
    sqlite3_bind_text (u, 2, new_status, -1, SQLITE_STATIC);
    sqlite3_bind_int64(u, 3, now_ms());
    sqlite3_bind_text (u, 4, client_id,  -1, SQLITE_STATIC);
    sqlite3_step(u);
    sqlite3_finalize(u);

    printf("[db] %s %s (exchange_order_id=%s)\n", new_status, client_id, exch_id);
    cJSON_Delete(root);
    return 1;
}


/* ===== strategy ========================================================= */
/* average() — simple mean of `len` doubles starting at a[0]. */
double average(const double *a, int len) {
    double s = 0.0;
    for (int i = 0; i < len; i++) s += a[i];
    return s / len;
}


/* ===== main (the bot) =================================================== */
int main(void) {
    srand(time(NULL));   // seed randomness once (else rand() repeats every run)

    /* ---------- setup: credentials + database ---------- */
    const char *api_key = getenv("BINANCE_API_KEY");
    const char *secret  = getenv("BINANCE_API_SECRET");
    if (!api_key || !secret) { fprintf(stderr, "Missing API credentials\n"); return 1; }

    sqlite3 *db;
    if (sqlite3_open("trading.db", &db) != SQLITE_OK) {
        fprintf(stderr, "open failed: %s\n", sqlite3_errmsg(db)); return 1;
    }
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS orders ("
        " client_order_id TEXT PRIMARY KEY, exchange_order_id TEXT,"
        " symbol TEXT, side TEXT, type TEXT, quantity REAL, status TEXT,"
        " created_at_ms INTEGER, updated_at_ms INTEGER);", NULL, NULL, NULL);

    /* ---------- strategy loop ----------
     * Poll price -> update moving averages -> trade on a fast/slow crossover.
     * Every trade still goes through place_order()'s safety gates. Stop the bot
     * with Ctrl-C, or `touch KILL` (which both halts trades AND ends the loop). */
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
            hist[n++] = price;                                    // still filling up
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
                place_order(db, api_key, secret, "BTCUSDT", side, ORDER_QTY);
            }
            prev_fast_above = fast_above;
        }

        sleep(POLL_SECONDS);
    }

    sqlite3_close(db);
    return 0;
}
