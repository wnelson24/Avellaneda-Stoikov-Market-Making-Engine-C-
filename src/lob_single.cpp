#include <algorithm>
#include <deque>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <cmath>
#include <chrono>
#include <thread>   // sleep_for
using namespace std;

// ===== types =====
using Price = long long;      // integer price ticks
using Quantity = long long;
using Timestamp = long long;

enum class Side { BID, ASK };

struct Order {
    string id;
    Side side;
    Price px;
    Quantity qty;
    Timestamp ts;
    bool is_our_quote = false;
};

struct Fill {
    Side side;
    Quantity qty;
    Price px;
};

// ===== CLI globals (tunable) =====
static double TICK_USD   = 0.01; // $ value per price tick
static double DELTA      = 0.5;  // half-spread in ticks (base)
static int    QTY_BASE   = 2;    // base quote size
static int    QTY_MIN    = 1;    // minimum quote size
static double LAMBDA     = 0.05; // inventory tilt r = mid - LAMBDA*inv
static long   INV_SOFT   = 50;   // soft inventory limit
static long   INV_HARD   = 80;   // hard inventory limit
static double MAX_DD_USD = 200.0;// risk-off drawdown threshold ($)
static int    REFRESH_MS = 80;   // dashboard refresh pacing (ms)

// Optional synthetic flow (defaults off -> rely on CSV)
static double BUY_RATE   = 0.0;  // probability of buy aggressor per tick
static double SELL_RATE  = 0.0;  // probability of sell aggressor per tick
static long   MAX_SYN_Q  = 3;    // max synthetic trade size

// ===== arg helpers =====
static int   arg_int (int argc, char** argv, const string& name, int defv) {
    for (int i=1;i<argc-1;i++) if (string(argv[i])==name) return stoi(argv[i+1]);
    return defv;
}
static double arg_double(int argc, char** argv, const string& name, double defv) {
    for (int i=1;i<argc-1;i++) if (string(argv[i])==name) return stod(argv[i+1]);
    return defv;
}

// ===== OrderBook =====
class OrderBook {
public:
    void add_order(const Order& o) {
        if (o.side == Side::BID) { bids_[o.px].push_back(o); }
        else { asks_[o.px].push_back(o); }
        index_[o.id] = {o.side, o.px};
    }

    bool cancel_order(const string& id) {
        auto it = index_.find(id);
        if (it == index_.end()) return false;
        auto [side, px] = it->second;

        if (side == Side::BID) {
            auto qit = bids_.find(px);
            if (qit == bids_.end()) return false;
            auto& q = qit->second;
            for (auto dq = q.begin(); dq != q.end(); ++dq) {
                if (dq->id == id) {
                    q.erase(dq);
                    if (q.empty()) bids_.erase(qit);
                    index_.erase(it);
                    return true;
                }
            }
        } else {
            auto qit = asks_.find(px);
            if (qit == asks_.end()) return false;
            auto& q = qit->second;
            for (auto dq = q.begin(); dq != q.end(); ++dq) {
                if (dq->id == id) {
                    q.erase(dq);
                    if (q.empty()) asks_.erase(qit);
                    index_.erase(it);
                    return true;
                }
            }
        }
        return false;
    }

    // External aggressor hits resting book; return fills of OUR orders
    vector<Fill> external_trade(Side aggressor, Quantity qty) {
        vector<Fill> out;
        if (aggressor == Side::BID) {
            // hit asks
            while (qty > 0 && !asks_.empty()) {
                auto best_it = asks_.begin();
                auto level_px = best_it->first;
                auto& q = best_it->second;
                while (qty > 0 && !q.empty()) {
                    auto& front = q.front();
                    auto take = min(qty, front.qty);
                    if (front.is_our_quote) out.push_back(Fill{ front.side, take, level_px });
                    front.qty -= take; qty -= take;
                    if (front.qty == 0) q.pop_front();
                }
                if (q.empty()) asks_.erase(best_it);
            }
        } else {
            // hit bids
            while (qty > 0 && !bids_.empty()) {
                auto best_it = bids_.begin();
                auto level_px = best_it->first;
                auto& q = best_it->second;
                while (qty > 0 && !q.empty()) {
                    auto& front = q.front();
                    auto take = min(qty, front.qty);
                    if (front.is_our_quote) out.push_back(Fill{ front.side, take, level_px });
                    front.qty -= take; qty -= take;
                    if (front.qty == 0) q.pop_front();
                }
                if (q.empty()) bids_.erase(best_it);
            }
        }
        return out;
    }

    // Cross => taker, else rest as maker
    pair<Quantity, Price> place_quote_and_sim_fill(const Order& o, Price pre_best_opposite) {
        Quantity filled = 0;
        Price exec_px = pre_best_opposite;
        if (o.side == Side::BID) {
            if (!asks_.empty() && o.px >= asks_.begin()->first) {
                Quantity remain = o.qty;
                while (remain > 0 && !asks_.empty()) {
                    auto it = asks_.begin();
                    auto& q = it->second;
                    while (remain > 0 && !q.empty()) {
                        auto& front = q.front();
                        auto take = min(remain, front.qty);
                        front.qty -= take; remain -= take; filled += take;
                        if (front.qty == 0) q.pop_front();
                    }
                    if (q.empty()) asks_.erase(it); else break;
                }
                return {filled, exec_px};
            }
        } else { // ask
            if (!bids_.empty() && o.px <= bids_.begin()->first) {
                Quantity remain = o.qty;
                while (remain > 0 && !bids_.empty()) {
                    auto it = bids_.begin();
                    auto& q = it->second;
                    while (remain > 0 && !q.empty()) {
                        auto& front = q.front();
                        auto take = min(remain, front.qty);
                        front.qty -= take; remain -= take; filled += take;
                        if (front.qty == 0) q.pop_front();
                    }
                    if (q.empty()) bids_.erase(it); else break;
                }
                return {filled, exec_px};
            }
        }
        Order ours = o; ours.is_our_quote = true;
        add_order(ours);
        return {0, 0};
    }

    optional<Price> best_bid() const { if (bids_.empty()) return nullopt; return bids_.begin()->first; }
    optional<Price> best_ask() const { if (asks_.empty()) return nullopt; return asks_.begin()->first; }

private:
    using LevelQueue = deque<Order>;
    map<Price, LevelQueue, greater<Price>> bids_; // best at begin()
    map<Price, LevelQueue, less<Price>>    asks_; // best at begin()
    unordered_map<string, pair<Side, Price>> index_;
};

// ===== helpers & state =====
static Side parse_side(const string& s) { return (s == "BID") ? Side::BID : Side::ASK; }

struct State {
    long long inv_ticks = 0;   // inventory (units)
    long long cash_ticks = 0;  // cash in price ticks
    double sigma2 = 0.0;       // (reserved for vol; not used in risk logic below)
    optional<double> prev_mid;
    long long peak_pnl_ticks = 0; // for drawdown tracking
};

// RNG for optional synthetic flow
static uint64_t rng_state = 88172645463393265ull;
static inline uint64_t rng() { rng_state ^= rng_state << 7; rng_state ^= rng_state >> 9; return rng_state; }
static inline double urand01() { return (rng() >> 11) * (1.0/9007199254740992.0); }
static inline long   randint(long lo, long hi) { return lo + (long)(urand01() * (double)(hi - lo + 1)); }

// ANSI colors
static const string GREEN = "\033[32m";
static const string RED   = "\033[31m";
static const string YEL   = "\033[33m";
static const string CYAN  = "\033[36m";
static const string RST   = "\033[0m";

// Dynamic size: shrink as |inv| grows; never below QTY_MIN
static int size_for_inventory(long long inv) {
    double scale = max(0.2, 1.0 - (double)std::abs(inv) / (double)max<long>(1, INV_SOFT)); // 1 → 0.2
    int s = (int)floor(QTY_BASE * scale);
    return max(QTY_MIN, s);
}

// Inventory-aware quotes: r = mid - LAMBDA * inv; optionally gate sides near limits
struct Quotes {
    Price bid_px;
    Price ask_px;
    bool  enable_bid;
    bool  enable_ask;
    int   qty_bid;
    int   qty_ask;
};

static Quotes compute_quotes_inventory_aware(Price best_bid, Price best_ask, const State& st, bool risk_off) {
    const double mid = 0.5 * (best_bid + best_ask);
    const double r   = mid - LAMBDA * (double)st.inv_ticks;

    Price bid_q = (Price)floor(r - DELTA);
    Price ask_q = (Price)ceil (r + DELTA);

    // never cross
    if (bid_q > best_bid) bid_q = best_bid;
    if (ask_q < best_ask) ask_q = best_ask;

    bool enable_bid = true, enable_ask = true;

    // Side gating near limits (and in risk-off)
    if (std::abs(st.inv_ticks) >= INV_SOFT || risk_off) {
        if (st.inv_ticks > 0) {
            // long: disable bid (would add more long), keep ask (to sell down)
            enable_bid = false;
        } else if (st.inv_ticks < 0) {
            // short: disable ask (would add more short), keep bid (to buy back)
            enable_ask = false;
        }
        // pull the reducing side tighter (help get out of risk faster)
        if (!enable_bid) { // only ask
            ask_q = max<Price>(ask_q - 1, best_ask); // one tick inside
        }
        if (!enable_ask) { // only bid
            bid_q = min<Price>(bid_q + 1, best_bid); // one tick inside
        }
    }

    int q_base = size_for_inventory(st.inv_ticks);
    int qty_bid = q_base;
    int qty_ask = q_base;

    // If very close to hard limit, bias sizes: bigger on reducing side, minimal on risky side
    if (std::abs(st.inv_ticks) >= INV_SOFT) {
        if (st.inv_ticks > 0) { // long → want sells
            qty_ask = max(q_base, q_base + 1);
            qty_bid = QTY_MIN;
        } else if (st.inv_ticks < 0) { // short → want buys
            qty_bid = max(q_base, q_base + 1);
            qty_ask = QTY_MIN;
        }
    }

    return Quotes{bid_q, ask_q, enable_bid, enable_ask, qty_bid, qty_ask};
}

static void print_dashboard(long long ts, long long bb, long long ba, double mid_usd,
                            long long inv, long long cash_ticks, long long pnl_ticks,
                            long long trades, long long buys, long long sells,
                            bool risk_off)
{
    const double cash_usd = cash_ticks * TICK_USD;
    const double pnl_usd  = pnl_ticks  * TICK_USD;

    cout << "\033[2J\033[H"; // clear + home
    cout << fixed << setprecision(2);
    string pnl_color = (pnl_usd > 0 ? GREEN : (pnl_usd < 0 ? RED : RST));
    string mode = risk_off ? (YEL + string("RISK-OFF") + RST) : (CYAN + string("RUN") + RST);

    cout << "┌──────────── Market Making Dashboard ────────────┐\n";
    cout << "│ ts=" << ts
         << "   BB=$" << bb * TICK_USD
         << "   BA=$" << ba * TICK_USD
         << "   mid=$" << mid_usd << " │\n";
    cout << "│ inv=" << inv
         << "   cash=$" << cash_usd
         << "   pnl=" << pnl_color << "$" << pnl_usd << RST
         << "   trades=" << trades << " (B:" << buys << " S:" << sells << ") │\n";
    cout << "│ mode=" << mode
         << "   limits[soft=" << INV_SOFT << ", hard=" << INV_HARD << "]"
         << "   delta=" << DELTA << "   qty_base=" << QTY_BASE << " │\n";
    cout << "└────────────────────────────────────────────────┘\n";
}

int main(int argc, char** argv) {
    // CLI knobs
    TICK_USD   = arg_double(argc, argv, "--tick",     0.01);
    DELTA      = arg_double(argc, argv, "--delta",    0.5);
    QTY_BASE   = arg_int   (argc, argv, "--qty",      2);
    QTY_MIN    = arg_int   (argc, argv, "--qtymin",   1);
    LAMBDA     = arg_double(argc, argv, "--lambda",   0.05);
    INV_SOFT   = arg_int   (argc, argv, "--invsoft",  50);
    INV_HARD   = arg_int   (argc, argv, "--invhard",  80);
    MAX_DD_USD = arg_double(argc, argv, "--maxdd",    200.0);
    REFRESH_MS = arg_int   (argc, argv, "--sleep",    80);

    // optional synthetic flow for balance
    BUY_RATE   = arg_double(argc, argv, "--buyrate",  0.0);
    SELL_RATE  = arg_double(argc, argv, "--sellrate", 0.0);
    MAX_SYN_Q  = arg_int   (argc, argv, "--maxsynq",  3);

    OrderBook ob;
    State S;

    ofstream fout("data/results_usd.csv");
    fout << "ts,bb_usd,ba_usd,mid_usd,inv,cash_usd,pnl_usd,trades,buys,sells,mode\n";

    long long trades_total = 0, buys_total = 0, sells_total = 0;
    long long ts_counter = 0;

    bool risk_off = false;

    // Loop forever (replays CSV; keeps strategy state to feel continuous)
    while (true) {
        ifstream fin("data/sample_ticks.csv");
        if (!fin) { cerr << "Open data/sample_ticks.csv failed\n"; return 1; }
        string line; getline(fin, line); // header

        // reset book each pass; keep inv/cash
        ob = OrderBook();

        while (getline(fin, line)) {
            // --- parse event row ---
            stringstream ss(line);
            string ts_s,event,side_s,px_s,qty_s,id_s;
            getline(ss, ts_s, ',');
            getline(ss, event, ',');
            getline(ss, side_s, ',');
            getline(ss, px_s, ',');
            getline(ss, qty_s, ',');
            getline(ss, id_s, ',');

            ts_counter = (ts_s.empty() ? ts_counter + 1 : stoll(ts_s));

            // --- apply event to book ---
            if (event == "ADD") {
                ob.add_order(Order{ id_s, parse_side(side_s), stoll(px_s), stoll(qty_s), ts_counter, false });
            } else if (event == "CANCEL") {
                ob.cancel_order(id_s);
            } else if (event == "TRADE") {
                auto fills = ob.external_trade(parse_side(side_s), stoll(qty_s));
                for (const auto& f : fills) {
                    if (f.side == Side::BID) { // our bid got hit -> BUY
                        S.inv_ticks += f.qty;
                        S.cash_ticks -= static_cast<long long>(f.qty * f.px);
                        trades_total++; buys_total++;
                    } else {                   // our ask got hit -> SELL
                        S.inv_ticks -= f.qty;
                        S.cash_ticks += static_cast<long long>(f.qty * f.px);
                        trades_total++; sells_total++;
                    }
                }
            }

            auto bb = ob.best_bid(), ba = ob.best_ask();
            if (!bb || !ba) continue;

            const double mid = 0.5 * (*bb + *ba);

            // --- optional synthetic flow to balance fills ---
            if (BUY_RATE > 0.0 && urand01() < BUY_RATE) {
                auto fills = ob.external_trade(Side::BID, randint(1, MAX_SYN_Q));
                for (const auto& f : fills) { // same accounting
                    if (f.side == Side::BID) { S.inv_ticks += f.qty; S.cash_ticks -= (long long)(f.qty * f.px); trades_total++; buys_total++; }
                    else                      { S.inv_ticks -= f.qty; S.cash_ticks += (long long)(f.qty * f.px); trades_total++; sells_total++; }
                }
            }
            if (SELL_RATE > 0.0 && urand01() < SELL_RATE) {
                auto fills = ob.external_trade(Side::ASK, randint(1, MAX_SYN_Q));
                for (const auto& f : fills) {
                    if (f.side == Side::BID) { S.inv_ticks += f.qty; S.cash_ticks -= (long long)(f.qty * f.px); trades_total++; buys_total++; }
                    else                      { S.inv_ticks -= f.qty; S.cash_ticks += (long long)(f.qty * f.px); trades_total++; sells_total++; }
                }
            }

            // --- compute dynamic risk state (drawdown + limits) ---
            long long pnl_ticks = S.cash_ticks + (long long)llround(S.inv_ticks * mid);
            S.peak_pnl_ticks = max(S.peak_pnl_ticks, pnl_ticks);
            double dd_usd = (S.peak_pnl_ticks - pnl_ticks) * TICK_USD;

            risk_off = (std::abs(S.inv_ticks) >= INV_HARD) || (dd_usd >= MAX_DD_USD);

            // --- compute quotes (inventory-aware + side gating) ---
            Quotes q = compute_quotes_inventory_aware(*bb, *ba, S, risk_off);

            // --- place quotes (only enabled sides), quantities dynamic ---
            if (q.enable_bid) {
                ob.place_quote_and_sim_fill(Order{ "qb"+to_string(ts_counter), Side::BID, q.bid_px, q.qty_bid, ts_counter, true }, *ba);
            }
            if (q.enable_ask) {
                ob.place_quote_and_sim_fill(Order{ "qa"+to_string(ts_counter), Side::ASK, q.ask_px, q.qty_ask, ts_counter, true }, *bb);
            }

            // --- dashboard + CSV ---
            pnl_ticks = S.cash_ticks + (long long)llround(S.inv_ticks * mid);
            print_dashboard(ts_counter, *bb, *ba, mid * TICK_USD,
                            S.inv_ticks, S.cash_ticks, pnl_ticks,
                            trades_total, buys_total, sells_total,
                            risk_off);

            fout << fixed << setprecision(2)
                 << ts_counter << ","
                 << (*bb * TICK_USD) << ","
                 << (*ba * TICK_USD) << ","
                 << (mid * TICK_USD) << ","
                 << S.inv_ticks << ","
                 << (S.cash_ticks * TICK_USD) << ","
                 << (pnl_ticks * TICK_USD) << ","
                 << trades_total << ","
                 << buys_total << ","
                 << sells_total << ","
                 << (risk_off ? "RISK_OFF" : "RUN")
                 << "\n";

            std::this_thread::sleep_for(std::chrono::milliseconds(REFRESH_MS));
        }
        // loop again over CSV, keeping inv/cash for continuity
    }

    return 0;
}
