#include <iostream>
#include <string>
#include <cstdio>
#include <memory>
#include <thread>
#include <chrono>
#include <array>
#include <deque>
#include <vector>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <atomic>
#include <csignal>

using namespace std;
using namespace chrono;

//====================================================//
//                  SIGNAL HANDLER                    //
//====================================================//

atomic<bool> running{true};

void onSignal(int) {
    running = false;
    cout << "\n[SHUTDOWN] Gracefully stopping...\n";
}

//====================================================//
//                    UTILITIES                       //
//====================================================//

string exec(const string& cmd) {
    array<char, 512> buffer;
    string result;
    unique_ptr<FILE, decltype(&pclose)>
        pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) return "";
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr)
        result += buffer.data();
    return result;
}

// More robust: tries multiple JSON key variants
double extractValue(const string& json, const string& key) {
    size_t pos = json.find(key);
    if (pos == string::npos) return -1.0;
    pos += key.length();
    // Skip whitespace/colon if needed
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':'))
        pos++;
    char* end;
    double val = strtod(json.c_str() + pos, &end);
    return (end == json.c_str() + pos) ? -1.0 : val;
}

double extractPrice(const string& r)     { return extractValue(r, "\"regularMarketPrice\":"); }
double extractVolume(const string& r)    { return extractValue(r, "\"regularMarketVolume\":"); }
double extractDayHigh(const string& r)   { return extractValue(r, "\"regularMarketDayHigh\":"); }
double extractDayLow(const string& r)    { return extractValue(r, "\"regularMarketDayLow\":"); }
double extractPrevClose(const string& r) { return extractValue(r, "\"regularMarketPreviousClose\":"); }

struct Tick {
    double price     = 0;
    double volume    = 0;
    double dayHigh   = 0;
    double dayLow    = 0;
    double prevClose = 0;
    steady_clock::time_point ts;
};

// Fetch + parse into Tick in one call
bool fetchTick(const string& symbol, Tick& out) {
    static const string base =
        "curl -s --connect-timeout 3 --max-time 5 "
        "-H \"User-Agent: Mozilla/5.0\" "
        "\"https://query1.finance.yahoo.com/v7/finance/quote?symbols=";

    string response = exec(base + symbol + "\"");
    if (response.empty()) return false;

    double p = extractPrice(response);
    if (p <= 0) return false;

    out.price     = p;
    out.volume    = extractVolume(response);
    out.dayHigh   = extractDayHigh(response);
    out.dayLow    = extractDayLow(response);
    out.prevClose = extractPrevClose(response);
    out.ts        = steady_clock::now();
    return true;
}

//====================================================//
//                  ROLLING INDICATORS                //
//====================================================//

// Generic rolling window helper
class RollingWindow {
    int cap;
    deque<double> buf;
    double sum_ = 0;
public:
    RollingWindow(int n) : cap(n) {}

    void push(double v) {
        buf.push_back(v);
        sum_ += v;
        if ((int)buf.size() > cap) { sum_ -= buf.front(); buf.pop_front(); }
    }

    bool ready()          const { return (int)buf.size() == cap; }
    int  size()           const { return (int)buf.size(); }
    double sum()          const { return sum_; }
    double mean()         const { return ready() ? sum_ / cap : 0.0; }
    const deque<double>& data() const { return buf; }

    double stdev() const {
        if (!ready()) return 0.0;
        double m = mean();
        double var = 0;
        for (double v : buf) var += (v - m) * (v - m);
        return sqrt(var / cap);
    }

    double back()  const { return buf.empty() ? 0 : buf.back(); }
    double front() const { return buf.empty() ? 0 : buf.front(); }
};

//----------------------------------------------------//
// EMA (Exponential Moving Average)
//----------------------------------------------------//
class EMA {
    int period;
    double alpha;
    double val = 0;
    bool ready_ = false;
    int count = 0;
    double warmup = 0;
public:
    EMA(int p) : period(p), alpha(2.0 / (p + 1)) {}

    double update(double v) {
        if (!ready_) {
            warmup += v;
            count++;
            if (count == period) {
                val = warmup / period;
                ready_ = true;
            }
            return 0.0;
        }
        val = alpha * v + (1 - alpha) * val;
        return val;
    }

    bool ready() const { return ready_; }
    double get() const { return val; }
};

//----------------------------------------------------//
// RSI (Relative Strength Index)
//----------------------------------------------------//
class RSI {
    int period;
    double avgGain = 0, avgLoss = 0;
    double prevPrice = -1;
    int count = 0;
    bool ready_ = false;
public:
    RSI(int p = 14) : period(p) {}

    double update(double price) {
        if (prevPrice < 0) { prevPrice = price; return 50.0; }

        double change = price - prevPrice;
        double gain = max(change, 0.0);
        double loss = max(-change, 0.0);
        prevPrice = price;

        if (!ready_) {
            avgGain = (avgGain * count + gain) / (count + 1);
            avgLoss = (avgLoss * count + loss) / (count + 1);
            count++;
            if (count >= period) ready_ = true;
        } else {
            // Wilder smoothing
            avgGain = (avgGain * (period - 1) + gain) / period;
            avgLoss = (avgLoss * (period - 1) + loss) / period;
        }

        if (avgLoss == 0) return 100.0;
        double rs = avgGain / avgLoss;
        return 100.0 - (100.0 / (1.0 + rs));
    }

    bool ready() const { return ready_; }
};

//----------------------------------------------------//
// Bollinger Bands
//----------------------------------------------------//
struct BBands {
    double upper, mid, lower, bandwidth, pctB;
};

BBands bollingerBands(const RollingWindow& w, double kMult = 2.0) {
    double mid   = w.mean();
    double sd    = w.stdev();
    double upper = mid + kMult * sd;
    double lower = mid - kMult * sd;
    double bw    = (mid > 0) ? (upper - lower) / mid : 0;
    double price = w.back();
    double pctB  = (upper != lower) ? (price - lower) / (upper - lower) : 0.5;
    return {upper, mid, lower, bw, pctB};
}

//----------------------------------------------------//
// ATR (Average True Range) — uses real OHLC when available
//----------------------------------------------------//
class ATR {
    int period;
    double atr = 0;
    int count = 0;
    bool ready_ = false;
public:
    ATR(int p = 14) : period(p) {}

    double update(double high, double low, double prevClose) {
        double tr = max({high - low,
                         abs(high - prevClose),
                         abs(low  - prevClose)});
        if (!ready_) {
            atr = (atr * count + tr) / (count + 1);
            count++;
            if (count >= period) ready_ = true;
        } else {
            atr = (atr * (period - 1) + tr) / period;
        }
        return atr;
    }

    double get()  const { return atr; }
    bool ready()  const { return ready_; }
};

//====================================================//
//               VOLUME ANALYSIS                      //
//====================================================//

// Detect if current volume is above its rolling average
bool isVolumeSpike(const RollingWindow& volWindow, double currentVol, double mult = 1.5) {
    if (!volWindow.ready()) return false;
    return currentVol > volWindow.mean() * mult;
}

//====================================================//
//              SIGNAL SCORING ENGINE                 //
//====================================================//

/*
    Confluent signal approach:
    Multiple conditions vote; trade only on strong consensus.
    Score range: -4 (strong sell) to +4 (strong buy)
*/
struct SignalScore {
    int score = 0;           // composite vote
    string reasons;          // human-readable explanation

    void add(int vote, const string& label) {
        score += vote;
        reasons += (vote > 0 ? "[+" : "[") + to_string(vote) + " " + label + "] ";
    }
};

SignalScore computeSignal(
    double price,
    double fastEMA, double slowEMA,
    double rsi,
    const BBands& bb,
    double atr,
    double volume, const RollingWindow& volWin,
    double prevPrice
) {
    SignalScore sig;

    // 1. EMA crossover trend
    if (fastEMA > 0 && slowEMA > 0) {
        double gap = (fastEMA - slowEMA) / price;
        if (gap > 0.001)       sig.add(+1, "EMA_bull");
        else if (gap < -0.001) sig.add(-1, "EMA_bear");
    }

    // 2. RSI — overbought/oversold
    if (rsi < 30)      sig.add(+1, "RSI_oversold");
    else if (rsi > 70) sig.add(-1, "RSI_overbought");
    else if (rsi > 55) sig.add(+1, "RSI_bullish");
    else if (rsi < 45) sig.add(-1, "RSI_bearish");

    // 3. Bollinger %B
    if (bb.pctB < 0.1)      sig.add(+1, "BB_oversold");
    else if (bb.pctB > 0.9) sig.add(-1, "BB_overbought");
    else if (bb.pctB > 0.6) sig.add(+1, "BB_upper_zone");
    else if (bb.pctB < 0.4) sig.add(-1, "BB_lower_zone");

    // 4. Momentum (simple price change vs ATR)
    if (atr > 0 && prevPrice > 0) {
        double chg = (price - prevPrice) / atr;
        if (chg > 0.3)       sig.add(+1, "MOM_up");
        else if (chg < -0.3) sig.add(-1, "MOM_down");
    }

    return sig;
}

//====================================================//
//                 POSITION MANAGER                   //
//====================================================//

struct Position {
    double shares   = 0;
    double entry    = 0;
    double stopLoss = 0;
    double takeProfit = 0;
};

//====================================================//
//                   TRADE LOG                        //
//====================================================//

struct Trade {
    string type;       // BUY / SELL
    double price;
    double shares;
    double pnl;
    string time;
};

string nowStr() {
    auto t  = system_clock::to_time_t(system_clock::now());
    char buf[20];
    strftime(buf, sizeof(buf), "%H:%M:%S", localtime(&t));
    return buf;
}

//====================================================//
//                   MAIN BOT                         //
//====================================================//

int main(int argc, char* argv[]) {

    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);

    //------------------------------------------------//
    // CONFIG (easily tunable)
    //------------------------------------------------//
    const string symbol       = (argc > 1) ? argv[1] : "AMZN";
    const double startCash    = 10000.0;
    const double riskPerTrade = 0.01;      // 1% account risk per trade
    const double maxDrawdown  = 0.10;      // halt at 10% drawdown
    const int    pollSec      = 5;         // fetch interval (seconds)
    const int    cooldownSec  = 15;        // min seconds between trades
    const int    minBuyScore  = 2;         // need 2+ bullish votes to buy
    const int    minSellScore = -2;        // need -2 or less to exit

    //------------------------------------------------//
    // INDICATORS
    //------------------------------------------------//
    EMA           fastEMA(9);
    EMA           slowEMA(21);
    RSI           rsi14(14);
    RollingWindow bbWindow(20);
    RollingWindow volWindow(20);
    ATR           atr14(14);

    //------------------------------------------------//
    // STATE
    //------------------------------------------------//
    double cash       = startCash;
    double maxEquity  = startCash;
    Position pos;
    vector<Trade> trades;

    double prevPrice  = 0;
    int    tick       = 0;
    auto   lastTrade  = steady_clock::now() - seconds(cooldownSec + 1);

    cout << fixed << setprecision(4);
    cout << "╔══════════════════════════════════════════╗\n";
    cout << "║    TRADING BOT  |  Symbol: " << symbol
         << string(14 - symbol.size(), ' ') << "║\n";
    cout << "╚══════════════════════════════════════════╝\n\n";

    //------------------------------------------------//
    // MAIN LOOP
    //------------------------------------------------//
    while (running) {

        //--------------------------------------------//
        // 1. FETCH TICK
        //--------------------------------------------//
        Tick tick_data;
        if (!fetchTick(symbol, tick_data)) {
            cerr << "[WARN] Fetch failed, retrying...\n";
            this_thread::sleep_for(seconds(2));
            continue;
        }

        double price = tick_data.price;

        //--------------------------------------------//
        // 2. FEED INDICATORS
        //--------------------------------------------//
        double fema = fastEMA.update(price);
        double sema = slowEMA.update(price);
        double rsiVal = rsi14.update(price);

        bbWindow.push(price);
        BBands bb = bollingerBands(bbWindow);

        // Use real intraday high/low if available
        double high = (tick_data.dayHigh  > 0) ? tick_data.dayHigh  : price;
        double low  = (tick_data.dayLow   > 0) ? tick_data.dayLow   : price;
        double pc   = (tick_data.prevClose > 0) ? tick_data.prevClose : prevPrice;
        double atrVal = (pc > 0) ? atr14.update(high, low, pc) : 0;

        if (tick_data.volume > 0)
            volWindow.push(tick_data.volume);

        //--------------------------------------------//
        // 3. EQUITY & DRAWDOWN CHECK
        //--------------------------------------------//
        double equity = cash + pos.shares * price;
        maxEquity = max(maxEquity, equity);

        if ((maxEquity - equity) / maxEquity > maxDrawdown) {
            if (pos.shares > 0) {
                cash += pos.shares * price;
                trades.push_back({"EMERGENCY_EXIT", price, pos.shares,
                    (price - pos.entry) * pos.shares, nowStr()});
                pos = {};
            }
            cout << "\n⛔  MAX DRAWDOWN REACHED — BOT HALTED\n";
            break;
        }

        //--------------------------------------------//
        // 4. SIGNAL
        //--------------------------------------------//
        SignalScore sig = computeSignal(
            price, fema, sema, rsiVal, bb,
            atrVal, tick_data.volume, volWindow, prevPrice
        );

        bool cooldownOk = (steady_clock::now() - lastTrade) > seconds(cooldownSec);
        bool indicatorsReady = fastEMA.ready() && slowEMA.ready()
                            && rsi14.ready() && bbWindow.ready();

        //--------------------------------------------//
        // 5a. ENTRY — flat position only
        //--------------------------------------------//
        if (pos.shares == 0 && cooldownOk && indicatorsReady) {

            if (sig.score >= minBuyScore) {

                // ATR-based position sizing
                double stopDist = (atrVal > 0) ? atrVal * 2.0 : price * 0.01;
                double riskAmt  = equity * riskPerTrade;
                double shares   = min({riskAmt / stopDist,
                                       cash / price,
                                       100.0});

                if (shares > 0.01) {
                    pos.shares     = shares;
                    pos.entry      = price;
                    pos.stopLoss   = price - stopDist;
                    pos.takeProfit = price + stopDist * 3.0;  // 3:1 R/R
                    cash          -= shares * price;
                    lastTrade      = steady_clock::now();

                    trades.push_back({"BUY", price, shares, 0, nowStr()});

                    cout << "🟢 BUY  @ " << price
                         << " | Shares: "  << shares
                         << " | SL: "      << pos.stopLoss
                         << " | TP: "      << pos.takeProfit
                         << "\n   Signals: " << sig.reasons << "\n\n";
                }
            }
        }

        //--------------------------------------------//
        // 5b. EXIT — manage open position
        //--------------------------------------------//
        else if (pos.shares > 0) {

            // Trailing stop: raise floor as price climbs
            double newStop = price - atrVal * 2.0;
            if (newStop > pos.stopLoss)
                pos.stopLoss = newStop;

            bool hitTP    = price >= pos.takeProfit;
            bool hitSL    = price <= pos.stopLoss;
            bool signalExit = sig.score <= minSellScore;

            if (hitTP || hitSL || signalExit) {
                double proceeds = pos.shares * price;
                double pnl      = (price - pos.entry) * pos.shares;
                cash           += proceeds;

                string reason = hitTP ? "TAKE_PROFIT" : hitSL ? "STOP_LOSS" : "SIGNAL_EXIT";
                trades.push_back({reason, price, pos.shares, pnl, nowStr()});

                cout << (pnl >= 0 ? "🔵" : "🔴")
                     << " " << reason
                     << " @ " << price
                     << " | PnL: $" << pnl
                     << " | Reason: " << sig.reasons << "\n\n";

                pos = {};
                lastTrade = steady_clock::now();
            }
        }

        //--------------------------------------------//
        // 6. STATUS (every 3 ticks)
        //--------------------------------------------//
        if (tick % 3 == 0) {
            equity = cash + pos.shares * price;
            cout << "[" << nowStr() << "] "
                 << symbol << " $" << price
                 << " | EMA9: " << fema
                 << " | EMA21: " << sema
                 << " | RSI: " << rsiVal
                 << " | BB%: " << bb.pctB
                 << " | ATR: " << atrVal
                 << "\n"
                 << "         Cash: $" << cash
                 << " | Equity: $" << equity
                 << " | Score: "   << sig.score
                 << " | "          << (pos.shares > 0 ? "IN POSITION" : "FLAT")
                 << "\n\n";
        }

        prevPrice = price;
        tick++;
        this_thread::sleep_for(seconds(pollSec));
    }

    //------------------------------------------------//
    // FINAL SUMMARY
    //------------------------------------------------//
    cout << "\n══════════════ TRADE SUMMARY ══════════════\n";
    double totalPnL = 0;
    int wins = 0, losses = 0;
    for (auto& t : trades) {
        if (t.type != "BUY") {
            totalPnL += t.pnl;
            if (t.pnl >= 0) wins++; else losses++;
            cout << t.time << " " << t.type
                 << " @ $" << t.price
                 << " | PnL: $" << t.pnl << "\n";
        }
    }

    double finalEquity = cash + pos.shares * (prevPrice > 0 ? prevPrice : 0);
    cout << "────────────────────────────────────────────\n";
    cout << "Final equity : $" << finalEquity << "\n";
    cout << "Total PnL    : $" << totalPnL << "\n";
    cout << "Trades       : " << (wins + losses) << " | Wins: " << wins << " | Losses: " << losses << "\n";
    if (wins + losses > 0)
        cout << "Win rate     : " << (100.0 * wins / (wins + losses)) << "%\n";

    return 0;
}
