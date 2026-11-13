import argparse
import urllib.request
import json
import sys

def make_request(url, method="GET", payload=None):
    """Utility to make synchronous HTTP requests to BlockSprint API."""
    try:
        headers = {"Content-Type": "application/json"}
        data = json.dumps(payload).encode("utf-8") if payload else None
        req = urllib.request.Request(url, method=method, data=data, headers=headers)
        with urllib.request.urlopen(req, timeout=3) as res:
            return json.loads(res.read().decode("utf-8"))
    except Exception as e:
        print(f"\n[Error] Connection to BlockSprint failed: {e}")
        print("Please ensure the C++ matching engine server is running on Port 8080.")
        sys.exit(1)

def get_live_bbo(symbol):
    return make_request(f"http://127.0.0.1:8080/api/bbo?symbol={symbol}")

def get_live_depth(symbol):
    return make_request(f"http://127.0.0.1:8080/api/depth?symbol={symbol}")

def predict_matching_trades(side, order_type, price, quantity, depth):
    """
    Predicts matching trade fills by walking the active order book depth.
    Replicates C++ matching rules to predict price-time matching results.
    """
    trades = []
    remaining_qty = quantity
    
    if side == "buy":
        # Match against resting sell orders (asks)
        asks = depth.get("asks", [])
        for ask in asks:
            ask_price = float(ask[0])
            ask_qty = float(ask[1])
            
            # Limit price condition: Taker buy price must be >= Maker ask price
            if order_type != "market" and price < ask_price:
                break
                
            if remaining_qty <= 0:
                break
                
            match_qty = min(remaining_qty, ask_qty)
            trades.append({
                "price": ask_price,
                "quantity": match_qty
            })
            remaining_qty -= match_qty
            
    else: # sell side
        # Match against resting buy orders (bids)
        bids = depth.get("bids", [])
        for bid in bids:
            bid_price = float(bid[0])
            bid_qty = float(bid[1])
            
            # Limit price condition: Taker sell price must be <= Maker bid price
            if order_type != "market" and price > bid_price:
                break
                
            if remaining_qty <= 0:
                break
                
            match_qty = min(remaining_qty, bid_qty)
            trades.append({
                "price": bid_price,
                "quantity": match_qty
            })
            remaining_qty -= match_qty

    # Apply FOK (Fill Or Kill) check
    if order_type == "fok":
        if remaining_qty > 0:
            # Cannot be fully filled, so the order is killed (0 trades expected)
            return [], True # (trades, is_killed)
        
    return trades, False

def by_level(trades):
    """Total quantity per price. With connector v2 (P7.2) a price level is a FIFO queue of synthetic orders
    (the market's own queue), so one taker order makes one trade per order it meets, several at one price;
    the depth endpoint shows the level's total, which is what the prediction can know."""
    levels = {}
    for t in trades:
        key = round(t["price"], 8)
        levels[key] = levels.get(key, 0.0) + t["quantity"]
    return sorted(levels.items())


def ask(prompt, given):
    return given if given is not None else input(prompt)


def run_interactive_test(args=None):
    args = args or argparse.Namespace(symbol=None, side=None, type=None, price=None, qty=None)
    print("==================================================")
    print("      BlockSprint Interactive Trade Validator     ")
    print("==================================================")
    
    # 1. Ask for symbol
    symbol = ask("Enter Symbol (default: BTC-USDT): ", args.symbol).strip().upper()
    if not symbol:
        symbol = "BTC-USDT"
        
    # Get BBO to show the user the active market spread
    bbo = get_live_bbo(symbol)
    print(f"\n--- Current Spread for {symbol} ---")
    print(f"Best Bid: {bbo.get('bid_price')} (Qty: {bbo.get('bid_qty')})")
    print(f"Best Ask: {bbo.get('ask_price')} (Qty: {bbo.get('ask_qty')})")
    print("--------------------------------------------------")
    
    # 2. Get order details
    side = ask("Enter Order Side (BUY/SELL): ", args.side).strip().lower()
    if side not in ("buy", "sell"):
        print("[Error] Invalid side. Must be BUY or SELL.")
        return
        
    order_type = ask("Enter Order Type (limit/market/ioc/fok): ", args.type).strip().lower()
    if order_type not in ("limit", "market", "ioc", "fok"):
        print("[Error] Invalid order type.")
        return
        
    price = 0.0
    if order_type != "market":
        price_input = ask("Enter Limit Price: ", args.price).strip()
        try:
            price = float(price_input)
        except ValueError:
            print("[Error] Invalid price.")
            return
            
    qty_input = ask("Enter Quantity: ", args.qty).strip()
    try:
        qty = float(qty_input)
    except ValueError:
        print("[Error] Invalid quantity.")
        return
        
    # 3. Pull active book depth before submission
    depth = get_live_depth(symbol)
    
    # 4. Predict expected fills
    expected_trades, fok_killed = predict_matching_trades(side, order_type, price, qty, depth)
    
    # 5. Place the order
    order_payload = {
        "symbol": symbol,
        "side": side,
        "order_type": order_type,
        "quantity": str(qty)
    }
    if order_type != "market":
        order_payload["price"] = str(price)
        
    print(f"\nSubmitting {side.upper()} {order_type.upper()} order to matching engine...")
    res = make_request("http://127.0.0.1:8080/api/orders", method="POST", payload=order_payload)
    
    # 6. Parse actual fills returned by BlockSprint
    actual_trades = []
    if "trades" in res:
        for t in res["trades"]:
            actual_trades.append({
                "price": float(t.get("price")),
                "quantity": float(t.get("quantity"))
            })
            
    # 7. Print validation comparison report
    print("\n================== Verification Report ==================")
    print(f"Taker Order ID: {res.get('order_id')}")
    print(f"Status:         {res.get('status')}")
    
    print("\nPREDICTED TRADES:")
    if not expected_trades:
        print("  <No Trades Expected>" if not fok_killed else "  <FOK Order Killed - No Trades>")
    for t in expected_trades:
        print(f"  Price: {t['price']:.6f} | Qty: {t['quantity']:.6f}")
        
    print("\nACTUAL ENGINE TRADES:")
    if not actual_trades:
        print("  <No Trades Executed>")
    for t in actual_trades:
        print(f"  Price: {t['price']:.6f} | Qty: {t['quantity']:.6f}")
        
    # Check if expected and actual trades match, level by level (a level's queue can fill in several trades)
    expected_levels = by_level(expected_trades)
    actual_levels = by_level(actual_trades)
    match_ok = len(expected_levels) == len(actual_levels) and all(
        abs(ep - ap) <= 1e-6 and abs(eq - aq) <= 1e-6
        for (ep, eq), (ap, aq) in zip(expected_levels, actual_levels))
                
    print("\n---------------------------------------------------------")
    if match_ok:
        print("\033[92m[PASS] Interactive Test Passed: Invariant Check OK!\033[0m")
    else:
        print("\033[91m[FAIL] Mismatch Detected: Engine output deviated from rules!\033[0m")
    print("=========================================================")
    return match_ok

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Predicts an order's fills from the live depth and checks the engine's")
    parser.add_argument("--symbol")
    parser.add_argument("--side", help="buy or sell")
    parser.add_argument("--type", help="limit, market, ioc or fok")
    parser.add_argument("--price")
    parser.add_argument("--qty")
    try:
        ok = run_interactive_test(parser.parse_args())
        sys.exit(0 if ok else 1)
    except KeyboardInterrupt:
        print("\nExiting interactive validator.")
