# TCP-Relay
[![License: MIT](https://img.shields.io/badge/license-MIT-blue)](LICENSE)

A high-performance and easy-to-use TCP-Relay write in C++20 based on [ASIO](https://think-async.com/Asio/) and [Coroutines](https://en.cppreference.com/w/cpp/language/coroutines). In addition to basic TCP relay, it also supports relaying through intermediate proxies (such as HTTP-Proxy).

# Getting Started
## Build from source
This project relies on some C++20 features. The following compilers and their respective versions are supported:
- GCC >= 12.0
- Clang >= 14.0
- Microsoft Visual Studio (MSVC) >= 2019

``` bash
# clone
git clone --recursive https://github.com/lxrite/tcp-relay.git
cd tcp-relay

# build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Usage
``` bash
$ ./tcp-relay --help
Usage: tcp-relay [options]

options:
  -h, --help                  Show this help message and exit
  -v, --version               Print the program version and exit
  -l, --listen_addr string    Local address to listen on (default: 0.0.0.0)
  -p, --port number           Local port to listen on (default: 8886)
  -t, --target string         Taget address (host:port) to connect
  --timeout number            Connection timeout (in seconds) (default: 240)
  --via [none | http_proxy]   Transfer via other proxy (default: none)
  --http_proxy string         HTTP-Proxy address (host:port)
  --log_level string [trace | debug | info | warn | error | disable] Log level (default: info)
```

## Examples
``` bash
# basic
./tcp-relay -t 172.16.1.1:8080

# IPv6
./tcp-relay -t [fd12:3456:789a:bcde::1]:8080

# domain
./tcp-relay -t example.com:8080

# relay through HTTP intermediate proxy
./tcp-relay -t example.com:8080 --via http_proxy --http_proxy proxy.example.com:1234
```

## Using Docker
``` bash
# pull image
docker pull ghcr.io/lxrite/tcp-relay:latest

# run
docker run -d -p 8886:8886 ghcr.io/lxrite/tcp-relay tcp-relay -t 172.16.1.1:8080
```
