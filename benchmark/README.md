# TCP-Relay Benchmark Tools

Benchmark tools for measuring tcp-relay performance.

## Components

- **echo-server** - Simple TCP echo server (target for benchmarks)
- **benchmark-client** - Benchmark client with multiple test modes

## Building

Benchmark tools are optional and disabled by default. To build them, enable the `BUILD_BENCHMARK` option:

```bash
cd tcp-relay
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_BENCHMARK=ON
cmake --build build --config Release
```

## Usage

### Step 1: Start the Echo Server

```bash
./build/Release/echo-server -p 5001
```

### Step 2: Start tcp-relay

```bash
./build/Release/tcp-relay -p 8886 -t 127.0.0.1:5001 --log_level disable
```

### Step 3: Run Benchmarks

**Throughput Test** - Measure maximum data transfer rate:
```bash
./build/Release/benchmark-client -h 127.0.0.1 -p 8886 -m throughput -c 10 -d 10
```

**Latency Test** - Measure round-trip latency:
```bash
./build/Release/benchmark-client -h 127.0.0.1 -p 8886 -m latency -c 10 -d 10
```



## Command-line Options

### echo-server

| Option | Description | Default |
|--------|-------------|---------|
| `-p, --port` | Port to listen on | 5001 |
| `--threads` | Number of worker threads | 4 |

### benchmark-client

| Option | Description | Default |
|--------|-------------|---------|
| `-h, --host` | Target host | 127.0.0.1 |
| `-p, --port` | Target port | 8886 |
| `-m, --mode` | Test mode: throughput\|latency | throughput |
| `-c, --connections` | Number of concurrent connections | 10 |
| `-d, --duration` | Test duration in seconds | 10 |
| `-s, --message-size` | Message size in bytes | 4096 |
| `-t, --threads` | Number of client threads | 4 |

## Example Output

### Throughput Test
```
=== Throughput Test Results ===
Duration:        10.00 seconds
Total Data:      1234.56 MB
Throughput:      123.45 MB/s
Connections:     10
Errors:          0
```

### Latency Test
```
=== Latency Test Results ===
Duration:        10.00 seconds
Samples:         50000
Avg Latency:     123.45 us
Min Latency:     50.00 us
Max Latency:     500.00 us
P50 Latency:     100.00 us
P95 Latency:     200.00 us
P99 Latency:     300.00 us
Errors:          0
```



## Benchmark Scenarios

| Scenario | Command |
|----------|---------|
| Single connection, max throughput | `-m throughput -c 1 -s 65536` |
| High concurrency | `-m throughput -c 100 -s 4096` |
| Small messages latency | `-m latency -c 10 -s 64` |

