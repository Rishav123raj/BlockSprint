import subprocess
import time
import os
import sys

def main():
    print("==================================================")
    print("      BlockSprint Full Project Test Harness       ")
    print("==================================================")

    # 1. Compile C++ Engine
    print("\n[1/4] Running CMake Configuration...")
    try:
        subprocess.run(["cmake", "-B", "build"], check=True)
    except subprocess.CalledProcessError as e:
        print(f"[Error] CMake configuration failed: {e}")
        sys.exit(1)
        
    print("\n[2/4] Compiling matching engine (Release)...")
    try:
        subprocess.run(["cmake", "--build", "build", "--config", "Release"], check=True)
    except subprocess.CalledProcessError as e:
        print(f"[Error] Compilation failed: {e}")
        sys.exit(1)

    # 2. Spawn C++ Engine in background
    print("\n[3/4] Starting C++ Matching Engine in background...")
    engine_path = os.path.join("build", "Release", "crypto_matching_engine.exe")
    if not os.path.exists(engine_path):
        engine_path = os.path.join("build", "crypto_matching_engine")
        
    if not os.path.exists(engine_path):
        print(f"[Error] Could not find executable matching engine path: {engine_path}")
        sys.exit(1)
        
    engine_proc = subprocess.Popen([engine_path], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    # 3. Spawn Binance Sync Connector in background
    print("[4/4] Starting Binance Connector in background...")
    connector_proc = subprocess.Popen(
        ["uv", "run", "python", "-u", "scripts/binance_connector.py"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL
    )

    try:
        print("\nWaiting 6 seconds for engine to boot and bootstrap REST snapshots...")
        time.sleep(6)

        # 4. Launch interactive CLI test in the foreground
        print("\n==================================================")
        print("          Starting Interactive Validation         ")
        print("==================================================")
        subprocess.run(["uv", "run", "scripts/interactive_test.py"])

    finally:
        # Cleanup background processes when finished
        print("\n==================================================")
        print("Stopping background services...")
        try:
            engine_proc.terminate()
            connector_proc.terminate()
            
            # Wait for them to exit
            engine_proc.wait(timeout=3)
            connector_proc.wait(timeout=3)
        except Exception:
            # Force kill if needed
            engine_proc.kill()
            connector_proc.kill()
            
        print("Background services stopped. Goodbye!")

if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nHarness stopped. Exiting.")
