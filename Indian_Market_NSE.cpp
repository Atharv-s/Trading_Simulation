#include <iostream>
#include <string>
#include <cstdio>
#include <array>
#include <thread>
#include <chrono>

using namespace std;

// Run shell curl command
string exec(const char* cmd) {
    array<char, 128> buffer;
    string result;

    FILE* pipe = popen(cmd, "r");
    if (!pipe) return "";

    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        result += buffer.data();
    }

    pclose(pipe);
    return result;
}

// Get Yahoo price
double getYahooPrice(const string& symbol) {

    string cmd =
        "curl -s -H \"User-Agent: Mozilla/5.0\" "
        "\"https://query1.finance.yahoo.com/v8/finance/chart/" +
        symbol +
        "?interval=1m&range=1d\"";

    string response = exec(cmd.c_str());

    if (response.empty())
        return -1;

    string key = "\"regularMarketPrice\":";
    size_t pos = response.find(key);

    if (pos == string::npos)
        return -1;

    pos += key.length();

    size_t end = response.find(",", pos);

    if (end == string::npos)
        return -1;

    return stod(response.substr(pos, end - pos));
}


int main() {

    string symbol = "RELIANCE.NS";

    double cash = 10000;
    double shares = 0;
    double entry = 0;

    cout << "=== NSE SIM (Yahoo Finance) ===\n";

    while (true) {

        double price = getYahooPrice(symbol);

        if (price <= 0) {
            cout << "DATA ERROR\n";
            this_thread::sleep_for(chrono::seconds(2));
            continue;
        }

        if (shares == 0) {

            shares = (cash * 0.1) / price;
            cash -= shares * price;
            entry = price;

            cout << "BUY @ " << price << endl;
        }

        if (shares > 0 && price >= entry * 1.01) {

            cash += shares * price;
            shares = 0;

            cout << "SELL @ " << price << endl;
        }

        double equity = cash + shares * price;

        cout << "Price: " << price
             << " | Equity: " << equity << endl;

        this_thread::sleep_for(chrono::seconds(2));
    }

    return 0;
}
