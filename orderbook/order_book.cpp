#include <cstdint>
#include <vector>
#include <string>
#include <unordered_map>
#include <iostream>
#include <set>
using namespace std;

struct Order{
    uint64_t order_id;
    bool is_buy;
    double price;
    uint64_t quantity;
    uint64_t timestamp_ns;
};

struct PriceLevel{
    double price;
    uint64_t total_quantity;
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
        order_lookup[order.order_id] = order;
        if(order.is_buy){
            buy_order.insert(&order_lookup[order.order_id]);
        } else {
            sell_order.insert(&order_lookup[order.order_id]);
        }
        match();
    }

    // // Cancel an existing order by its ID
    bool cancel_order(uint64_t order_id){
        if(order_lookup.find(order_id) == order_lookup.end()){
            return false;
        }
        Order* ord = &(order_lookup[order_id]);
        if(ord->is_buy){
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
            if(ord->is_buy){
                buy_order.erase(ord);
            }else{
                sell_order.erase(ord);
            }

            ord->price = new_price;
            ord->quantity = new_quantity;

            if(ord->is_buy){
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
                current_bid_qty = order->quantity;
            } else {
                // Same price level, accumulate quantity
                current_bid_qty += order->quantity;
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
                current_ask_qty = order->quantity;
            } else {
                current_ask_qty += order->quantity;
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
                std::cout << "Bid orderId: " << order->order_id << std::endl;
                cout << "Price: " << order->price <<" , Quantity: "<<order->quantity <<endl;
                cout << "" <<endl;
                ++it1;
                ++count;
                if (count >= depth) break;
            }

            // Print from map2 if available
            if (it2 != sell_order.end()) {
                const Order* order = *it2;
                cout << "" <<endl;
                std::cout << "Ask orderId: " << order->order_id << std::endl;
                cout << "Price: " << order->price <<" , Quantity: "<<order->quantity <<endl;
                ++it2;
                ++count;
            }
            cout<<"===================="<<endl;
        }
    };

    void confirm_order(Order * bid, Order* ask){
        cout << "Executed " << "Bid Order of Id: "<< bid->order_id <<" and price: " << bid->price 
        << " for Ask order of Id: "<< ask->order_id <<" and price: " << ask->price << endl;
        if(bid->quantity < ask->quantity){
            buy_order.erase(bid);
            ask->quantity -= bid->quantity;
            order_lookup.erase(bid->order_id);
        }else if(bid->quantity > ask->quantity){
            sell_order.erase(ask);
            bid->quantity -= ask->quantity;
            order_lookup.erase(ask->order_id);
        }else if(bid->quantity == ask->quantity){
            buy_order.erase(bid);
            order_lookup.erase(bid->order_id);
            sell_order.erase(ask);
            order_lookup.erase(ask->order_id);
        }
    }


    void match(){
        while (!buy_order.empty() && !sell_order.empty()) {
            auto it1 = buy_order.begin();
            auto it2 = sell_order.begin();

            // Print from map1 if available
            Order* bid = *it1;
            Order* ask = *it2;

            if(bid->price >= ask->price){
                cout << "===============================" << endl;
                confirm_order(bid, ask);
                cout << "===============================" << endl;
            }else{
                return;
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
             << (ord.is_buy ? "Yes" : "No") << ", Price: " << ord.price
             << ", Quantity: " << ord.quantity << ", Timestamp: " << ord.timestamp_ns << endl;
        cout << "========================"<< endl;
    }
};


int main(){
    OrderBook orderBook;
    
    cout << "=== Testing Order Book Implementation ===" << endl << endl;
    
    // Test 1: Add some buy orders
    cout << "1. Adding buy orders:" << endl;
    Order buy1 = {1001, true, 50.25, 100, 1000000000}; // order_id, is_buy, price, quantity, timestamp
    Order buy4 = {1011, true, 50.25, 200, 1000000010}; // order_id, is_buy, price, quantity, timestamp
    Order buy2 = {1002, true, 50.50, 200, 1000000001}; // order_id, is_buy, price, quantity, timestamp
    Order buy3 = {1003, true, 50.00, 150, 1000000002}; // order_id, is_buy, price, quantity, timestamp
    
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
    Order sell1 = {2001, false, 51.00, 80, 1000000003}; // order_id, is_buy, price, quantity, timestamp
    Order sell2 = {2002, false, 51.25, 120, 1000000004}; // order_id, is_buy, price, quantity, timestamp
    Order sell3 = {2003, false, 50.75, 90, 1000000005}; // order_id, is_buy, price, quantity, timestamp
    Order sell4 = {2004, false, 50.95, 190, 1000000015}; // order_id, is_buy, price, quantity, timestamp
    
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
        cout << "  Price: " << bid.price << ", Quantity: " << bid.total_quantity << endl;
    }
    
    cout << "==========Top 4 Aggregated Asks:==========" << endl;
    cout << "" << endl;
    for (const auto& ask : asks) {
        cout << "  Price: " << ask.price << ", Quantity: " << ask.total_quantity << endl;
    }
    cout << endl;
    
    // Test 4: Add a sell order that will match
    cout << "4. Adding a sell order (id: 2005, qty: 50, price: 50.25) that should match:" << endl;
    Order sell_match = {2005, false, 50.25, 50, 1000000006}; // order_id, is_buy, price, quantity, timestamp
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
    Order aggressive_buy = {3001, true, 52.00, 200, 1000000007}; // order_id, is_buy, price, quantity, timestamp
    Order aggressive_sell = {3002, false, 49.00, 100, 1000000008}; // order_id, is_buy, price, quantity, timestamp
    
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