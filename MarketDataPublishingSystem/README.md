# Market Data Publishing System

Low-latency market data publishing system with shared memory and TCP transport.

## Build

```bash
cd MarketDataPublishingSystem
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

## Run

Open 3 terminals:

**Terminal 1 - Publisher:**
```bash
./publisher [port] [messages_per_sec] [cpu_core]
./publisher 9000 1000 0
```

**Terminal 2 - SHM Consumer:**
```bash
./shm_consumer [cpu_core]
./shm_consumer 1
```

**Terminal 3 - TCP Consumer:**
```bash
./tcp_consumer [port] [cpu_core]
./tcp_consumer 9000 2
```

## Stop

Press `Ctrl+C` in each terminal.

## Clean Shared Memory

Shared memory is automatically cleaned up when the publisher exits gracefully (Ctrl+C).

If the publisher crashes or is killed forcefully, manually remove:
```bash
sudo rm /dev/shm/market_data_ring_buffer
```
