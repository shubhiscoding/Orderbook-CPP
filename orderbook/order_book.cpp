#include <cstdint>
#include <vector>
#include <string>
#include <unordered_map>
#include <iostream>
#include <set>
using namespace std;

struct Order{
    uint64_t qty;
    uint64_t orderId;
    double price;
    bool isbuy;
    uint64_t timestamp_ns;
};

struct PriceLevel{
    double price;
    uint64_t qty;
};

struct DescendingCompare {
    bool operator()(const Order* lhs, const Order* rhs) const {
        if (lhs->price != rhs->price) {
            return lhs->price > rhs->price;
        }
        return lhs->timestamp_ns < rhs->timestamp_ns;
    }
};


struct AscendingCompare {
    bool operator()(const Order* lhs, const Order* rhs) const {
        if (lhs->price != rhs->price) {
            return lhs->price < rhs->price;
        }
        return lhs->timestamp_ns < rhs->timestamp_ns;
    }
};

class OrderBook {
    unordered_map<uint64_t, Order> order_lookup;
    set<Order*, DescendingCompare> buy_order;
    set<Order*, AscendingCompare> sell_order;
public:
    // Insert a new order into the book
    void add_order(const Order& order){
        order_lookup[order.orderId] = order;
        if(order.isbuy){
            buy_order.insert(&order_lookup[order.orderId]);
        } else {
            sell_order.insert(&order_lookup[order.orderId]);
        }
        match();
    }

    // // Cancel an existing order by its ID
    bool cancel_order(uint64_t order_id){
        if(order_lookup.find(order_id) == order_lookup.end()){
            return false;
        }
        Order* ord = &(order_lookup[order_id]);
        if(ord->isbuy){
            buy_order.erase(ord);
        }else{
            sell_order.erase(ord);
        }
        order_lookup.erase(order_id);
        return true;
    }

    // // Amend an existing order's price or quantity
    bool amend_order(uint64_t order_id, double new_price, uint64_t new_quantity){
        if(order_lookup.find(order_id) != order_lookup.end()){
            Order* ord = &(order_lookup[order_id]);
            if(ord->isbuy){
                buy_order.erase(ord);
            }else{
                sell_order.erase(ord);
            }

            ord->price = new_price;
            ord->qty = new_quantity;

            if(ord->isbuy){
                buy_order.insert(ord);
            }else{
                sell_order.insert(ord);
            }
            match();
            return true;
        }
        return false;
    }

    // // Get a snapshot of top N bid and ask levels (aggregated quantities)
    void get_snapshot(size_t depth, std::vector<PriceLevel>& bids, std::vector<PriceLevel>& asks) const{
        bids.clear();
        asks.clear();

        // Aggregate bids while iterating (already in descending price order)
        double current_bid_price = -1;
        uint64_t current_bid_qty = 0;
        size_t bid_levels_added = 0;
        
        for (auto it = buy_order.begin(); it != buy_order.end() && bid_levels_added < depth; ++it) {
            const Order* order = *it;
            
            if (order->price != current_bid_price) {
                // New price level
                if (current_bid_price != -1) {
                    // Add previous level
                    bids.push_back({current_bid_price, current_bid_qty});
                    bid_levels_added++;
                    if (bid_levels_added >= depth) break;
                }
                current_bid_price = order->price;
                current_bid_qty = order->qty;
            } else {
                // Same price level, accumulate quantity
                current_bid_qty += order->qty;
            }
        }
        
        // Don't forget the last level
        if (current_bid_price != -1 && bid_levels_added < depth) {
            bids.push_back({current_bid_price, current_bid_qty});
        }

        // Similar logic for asks
        double current_ask_price = -1;
        uint64_t current_ask_qty = 0;
        size_t ask_levels_added = 0;
        
        for (auto it = sell_order.begin(); it != sell_order.end() && ask_levels_added < depth; ++it) {
            const Order* order = *it;
            
            if (order->price != current_ask_price) {
                if (current_ask_price != -1) {
                    asks.push_back({current_ask_price, current_ask_qty});
                    ask_levels_added++;
                    if (ask_levels_added >= depth) break;
                }
                current_ask_price = order->price;
                current_ask_qty = order->qty;
            } else {
                current_ask_qty += order->qty;
            }
        }
        
        if (current_ask_price != -1 && ask_levels_added < depth) {
            asks.push_back({current_ask_price, current_ask_qty});
        }
    }
    // // Print current state of the order book
    void print_book(size_t depth = 10) const{
        auto it1 = buy_order.begin();
        auto it2 = sell_order.begin();
        size_t count = 0;

        while (count < depth && (it1 != buy_order.end() || it2 != sell_order.end())) {
            // Print from map1 if available
            if (it1 != buy_order.end()) {
                const Order* order = *it1;
                std::cout << "Bid orderId: " << order->orderId << std::endl;
                cout << "Price: " << order->price <<" , Quantity: "<<order->qty <<endl;
                cout << "" <<endl;
                ++it1;
                ++count;
                if (count >= depth) break;
            }

            // Print from map2 if available
            if (it2 != sell_order.end()) {
                const Order* order = *it2;
                cout << "" <<endl;
                std::cout << "Ask orderId: " << order->orderId << std::endl;
                cout << "Price: " << order->price <<" , Quantity: "<<order->qty <<endl;
                ++it2;
                ++count;
            }
            cout<<"===================="<<endl;
        }
    };

    void confirm_order(Order * bid, Order* ask){
        cout << "Executed " << "Bid Order of Id: "<< bid->orderId <<" and price: " << bid->price 
        << " for Ask order of Id: "<< ask->orderId <<" and price: " << ask->price << endl;
        if(bid->qty < ask->qty){
            buy_order.erase(bid);
            ask->qty -= bid->qty;
            order_lookup.erase(bid->orderId);
        }else if(bid->qty > ask->qty){
            sell_order.erase(ask);
            bid->qty -= ask->qty;
            order_lookup.erase(ask->orderId);
        }else if(bid->qty == ask->qty){
            buy_order.erase(bid);
            order_lookup.erase(bid->orderId);
            sell_order.erase(ask);
            order_lookup.erase(ask->orderId);
        }
    }


    void match(){
        while (!buy_order.empty() && !sell_order.empty()) {
            auto it1 = buy_order.begin();
            auto it2 = sell_order.begin();

            // Print from map1 if available
            Order* bid = *it1;
            Order* ask = *it2;

            if(bid->price < ask->price){
                return;
            }else{
                cout << "===============================" << endl;
                confirm_order(bid, ask);
                cout << "===============================" << endl;
            }
        }
    }

    void print_order(int orderId){
        if(order_lookup.find(orderId) == order_lookup.end()){
            cout << "Order ID " << orderId << " not found in the order book." << endl;
            return;
        }
        Order ord = order_lookup[orderId];
        cout << "========================"<< endl;
        cout << "Order ID: " << orderId << endl;
        cout << "Buy "
             << (ord.isbuy ? "Yes" : "No") << ", Price: " << ord.price
             << ", Quantity: " << ord.qty << ", Timestamp: " << ord.timestamp_ns << endl;
        cout << "========================"<< endl;
    }
};


int main(){
    OrderBook orderBook;
    
    cout << "=== Testing Order Book Implementation ===" << endl << endl;
    
    // Test 1: Add some buy orders
    cout << "1. Adding buy orders:" << endl;
    Order buy1 = {100, 1001, 50.25, true, 1000000000}; // Buy 100 @ 50.25
    Order buy4 = {200, 1011, 50.25, true, 1000000010}; // Buy 100 @ 50.25
    Order buy2 = {200, 1002, 50.50, true, 1000000001}; // Buy 200 @ 50.50
    Order buy3 = {150, 1003, 50.00, true, 1000000002}; // Buy 150 @ 50.00
    
    orderBook.add_order(buy1);
    orderBook.add_order(buy2);
    orderBook.add_order(buy3);
    orderBook.add_order(buy4);
    
    cout << "==========Book state after adding buy orders:==========" << endl;
    cout << "" << endl;
    orderBook.print_book(5);
    cout << endl;
    
    // Test 2: Add some sell orders (no matching yet)
    cout << "2. Adding sell orders (higher prices, no matches):" << endl;
    Order sell1 = {80, 2001, 51.00, false, 1000000003}; // Sell 80 @ 51.00
    Order sell2 = {120, 2002, 51.25, false, 1000000004}; // Sell 120 @ 51.25
    Order sell3 = {90, 2003, 50.75, false, 1000000005}; // Sell 90 @ 50.75
    Order sell4 = {190, 2004, 50.95, false, 1000000015}; // Sell 90 @ 50.75
    
    orderBook.add_order(sell1);
    orderBook.add_order(sell2);
    orderBook.add_order(sell3);
    orderBook.add_order(sell4);

    cout << "==========Book state after adding sell orders:==========" << endl;
    cout << "" << endl;
    orderBook.print_book(10);
    cout << endl;


    // Test 3: Test snapshot functionality
    cout << "3. Testing snapshot functionality:" << endl;
    vector<PriceLevel> bids, asks;
    orderBook.get_snapshot(4, bids, asks);

    cout << "==========Top 4 Aggregated Bids:==========" << endl;
    cout << "" << endl;
    for (const auto& bid : bids) {
        cout << "  Price: " << bid.price << ", Quantity: " << bid.qty << endl;
    }
    
    cout << "==========Top 4 Aggregated Asks:==========" << endl;
    cout << "" << endl;
    for (const auto& ask : asks) {
        cout << "  Price: " << ask.price << ", Quantity: " << ask.qty << endl;
    }
    cout << endl;
    
    // Test 4: Add a sell order that will match
    cout << "4. Adding a sell order (id: 2004, qty: 50, price: 50.25) that should match:" << endl;
    Order sell_match = {50, 2004, 50.25, false, 1000000006}; // Sell 50 @ 50.25
    orderBook.add_order(sell_match);
    
    cout << "==========Book state after matching:==========" << endl;
    cout << "" << endl;
    orderBook.print_book(10);
    cout << endl;
    
    // Test 5: Test order cancellation
    cout << "5. Testing order cancellation:" << endl;
    cout << "Canceling order ID 1001..." << endl;
    bool cancelled = orderBook.cancel_order(1001);
    cout << "Cancellation " << (cancelled ? "successful" : "failed") << endl;
    
    cout << "Trying to cancel non-existent order 9999..." << endl;
    cancelled = orderBook.cancel_order(9999);
    cout << "Cancellation " << (cancelled ? "successful" : "failed") << endl;
    
    cout << "==========Book state after cancellation:==========" << endl;
    cout << ""<< endl;
    orderBook.print_book(10);
    cout << endl;
    
    // Test 6: Test order amendment
    cout << "6. Testing order amendment:" << endl;
    cout << "Amending order (for ID 1002) - changing price to 49.75 and quantity to 300..." << endl;
    bool amended = orderBook.amend_order(1002, 49.75, 300);
    cout << "Amendment " << (amended ? "successful" : "failed") << endl;

    cout << "==========Book state after amendment:==========" << endl;
    cout << "" << endl;
    orderBook.print_book(10);
    cout << endl;
    
    // Test 7: Add more orders to trigger more matches
    cout << "7. Adding aggressive orders to trigger matches:" << endl;
    Order aggressive_buy = {200, 3001, 52.00, true, 1000000007}; // High buy price
    Order aggressive_sell = {100, 3002, 49.00, false, 1000000008}; // Low sell price
    
    cout << "Adding aggressive buy order (price: 52.00)..." << endl;
    orderBook.add_order(aggressive_buy);
    
    cout << "Adding aggressive sell order (price: 49.00)..." << endl;
    orderBook.add_order(aggressive_sell);
    
    cout << "==========Final book state:==========" << endl;
    cout << "" << endl;
    orderBook.print_book(10);
    
    cout << endl << "=== Order Book Testing Complete ===" << endl;
    
    return 0;
}