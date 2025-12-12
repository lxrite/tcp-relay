/*
 *    benchmark_client.cpp:
 *
 *    TCP benchmark client for testing tcp-relay performance.
 *
 *    Copyright (C) 2023-2025 Light Lin <lxrite@gmail.com> All Rights Reserved.
 *
 */

#include "benchmark.h"
#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/connect.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/signal_set.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/write.hpp>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

std::atomic<bool> g_running{true};
std::mutex g_stats_mutex;
Statistics g_stats;

// Throughput test: send data as fast as possible
asio::awaitable<void> throughput_worker(const BenchmarkConfig &config,
                                        std::size_t connection_id) {
  auto executor = co_await asio::this_coro::executor;
  asio::ip::tcp::resolver resolver(executor);
  asio::ip::tcp::socket socket(executor);

  try {
    auto endpoints = co_await resolver.async_resolve(
        config.host, std::to_string(config.port), asio::use_awaitable);
    co_await asio::async_connect(socket, endpoints, asio::use_awaitable);
    g_stats.add_connection();

    // Create random data buffer
    std::vector<char> send_buffer(config.message_size);
    std::vector<char> recv_buffer(config.message_size);
    std::mt19937 rng(static_cast<unsigned>(connection_id));
    for (auto &c : send_buffer) {
      c = static_cast<char>(rng() % 256);
    }

    while (g_running.load(std::memory_order_relaxed)) {
      // Send data
      std::size_t bytes_sent = 0;
      while (bytes_sent < send_buffer.size()) {
        auto [ec, n] = co_await socket.async_write_some(
            asio::buffer(send_buffer.data() + bytes_sent,
                         send_buffer.size() - bytes_sent),
            asio::as_tuple(asio::use_awaitable));
        if (ec) {
          g_stats.add_error();
          co_return;
        }
        bytes_sent += n;
        g_stats.add_bytes(n);
      }

      // Receive echoed data
      std::size_t bytes_recv = 0;
      while (bytes_recv < send_buffer.size()) {
        auto [ec, n] = co_await socket.async_read_some(
            asio::buffer(recv_buffer.data() + bytes_recv,
                         recv_buffer.size() - bytes_recv),
            asio::as_tuple(asio::use_awaitable));
        if (ec) {
          g_stats.add_error();
          co_return;
        }
        bytes_recv += n;
        g_stats.add_bytes(n);
      }
    }
  } catch (...) {
    g_stats.add_error();
  }
}

// Latency test: measure round-trip time for each message
asio::awaitable<void> latency_worker(const BenchmarkConfig &config,
                                     std::size_t connection_id) {
  auto executor = co_await asio::this_coro::executor;
  asio::ip::tcp::resolver resolver(executor);
  asio::ip::tcp::socket socket(executor);

  try {
    auto endpoints = co_await resolver.async_resolve(
        config.host, std::to_string(config.port), asio::use_awaitable);
    co_await asio::async_connect(socket, endpoints, asio::use_awaitable);
    g_stats.add_connection();

    std::vector<char> send_buffer(config.message_size);
    std::vector<char> recv_buffer(config.message_size);
    std::mt19937 rng(static_cast<unsigned>(connection_id));
    for (auto &c : send_buffer) {
      c = static_cast<char>(rng() % 256);
    }

    std::vector<double> local_samples;
    local_samples.reserve(10000);

    while (g_running.load(std::memory_order_relaxed)) {
      auto start = Timer::now();

      // Send
      std::size_t bytes_sent = 0;
      while (bytes_sent < send_buffer.size()) {
        auto [ec, n] = co_await socket.async_write_some(
            asio::buffer(send_buffer.data() + bytes_sent,
                         send_buffer.size() - bytes_sent),
            asio::as_tuple(asio::use_awaitable));
        if (ec) {
          g_stats.add_error();
          co_return;
        }
        bytes_sent += n;
      }

      // Receive
      std::size_t bytes_recv = 0;
      while (bytes_recv < send_buffer.size()) {
        auto [ec, n] = co_await socket.async_read_some(
            asio::buffer(recv_buffer.data() + bytes_recv,
                         recv_buffer.size() - bytes_recv),
            asio::as_tuple(asio::use_awaitable));
        if (ec) {
          g_stats.add_error();
          co_return;
        }
        bytes_recv += n;
      }

      auto end = Timer::now();
      auto latency_us =
          std::chrono::duration<double, std::micro>(end - start).count();
      local_samples.push_back(latency_us);
    }

    // Merge local samples into global stats
    std::lock_guard<std::mutex> lock(g_stats_mutex);
    for (double sample : local_samples) {
      g_stats.add_sample(sample);
    }
  } catch (...) {
    g_stats.add_error();
  }
}

void print_usage() {
#ifdef _WIN32
  const char *prog = "benchmark-client.exe";
#else
  const char *prog = "benchmark-client";
#endif
  std::cout
      << "Usage: " << prog << " [options]\n\n"
      << "TCP benchmark client for testing tcp-relay performance.\n\n"
      << "options:\n"
      << "  --help                  Show this help message and exit\n"
      << "  -h, --host string       Target host (default: 127.0.0.1)\n"
      << "  -p, --port number       Target port (default: 8886)\n"
      << "  -m, --mode string       Test mode: throughput|latency (default: "
         "throughput)\n"
      << "  -c, --connections num   Number of concurrent connections (default: "
         "10)\n"
      << "  -d, --duration num      Test duration in seconds (default: 10)\n"
      << "  -s, --message-size num  Message size in bytes (default: 4096)\n"
      << "  -t, --threads num       Number of client threads (default: 4)\n";
}

BenchmarkConfig parse_args(int argc, char **argv) {
  BenchmarkConfig config;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--help") {
      print_usage();
      std::exit(EXIT_SUCCESS);
    } else if (arg == "-h" || arg == "--host") {
      if (++i >= argc) {
        std::cerr << "Missing value for " << arg << std::endl;
        std::exit(EXIT_FAILURE);
      }
      config.host = argv[i];
    } else if (arg == "-p" || arg == "--port") {
      if (++i >= argc) {
        std::cerr << "Missing value for " << arg << std::endl;
        std::exit(EXIT_FAILURE);
      }
      config.port = static_cast<std::uint16_t>(std::stoul(argv[i]));
    } else if (arg == "-m" || arg == "--mode") {
      if (++i >= argc) {
        std::cerr << "Missing value for " << arg << std::endl;
        std::exit(EXIT_FAILURE);
      }
      config.mode = argv[i];
      if (config.mode != "throughput" && config.mode != "latency") {
        std::cerr << "Invalid mode: " << config.mode << std::endl;
        std::exit(EXIT_FAILURE);
      }
    } else if (arg == "-c" || arg == "--connections") {
      if (++i >= argc) {
        std::cerr << "Missing value for " << arg << std::endl;
        std::exit(EXIT_FAILURE);
      }
      config.num_connections = std::stoul(argv[i]);
    } else if (arg == "-d" || arg == "--duration") {
      if (++i >= argc) {
        std::cerr << "Missing value for " << arg << std::endl;
        std::exit(EXIT_FAILURE);
      }
      config.duration_seconds = std::stoul(argv[i]);
    } else if (arg == "-s" || arg == "--message-size") {
      if (++i >= argc) {
        std::cerr << "Missing value for " << arg << std::endl;
        std::exit(EXIT_FAILURE);
      }
      config.message_size = std::stoul(argv[i]);
    } else if (arg == "-t" || arg == "--threads") {
      if (++i >= argc) {
        std::cerr << "Missing value for " << arg << std::endl;
        std::exit(EXIT_FAILURE);
      }
      config.num_threads = std::stoul(argv[i]);
    } else {
      std::cerr << "Unknown argument: " << arg << std::endl;
      print_usage();
      std::exit(EXIT_FAILURE);
    }
  }
  return config;
}

int main(int argc, char **argv) {
  auto config = parse_args(argc, argv);

  std::printf("Benchmark Configuration:\n");
  std::printf("  Host:        %s\n", config.host.c_str());
  std::printf("  Port:        %u\n", config.port);
  std::printf("  Mode:        %s\n", config.mode.c_str());
  std::printf("  Connections: %zu\n", config.num_connections);
  std::printf("  Duration:    %zu seconds\n", config.duration_seconds);
  std::printf("  Msg Size:    %zu bytes\n", config.message_size);
  std::printf("  Threads:     %zu\n", config.num_threads);
  std::printf("\nStarting benchmark...\n");

  try {
    asio::io_context io_context(config.num_threads);

    // Timer to stop the benchmark
    asio::steady_timer stop_timer(io_context);
    stop_timer.expires_after(std::chrono::seconds(config.duration_seconds));
    stop_timer.async_wait([&](auto) {
      g_running.store(false, std::memory_order_relaxed);
      io_context.stop();
    });

    // Signal handler
    asio::signal_set signals(io_context, SIGINT, SIGTERM);
    signals.async_wait([&](auto, auto) {
      g_running.store(false, std::memory_order_relaxed);
      io_context.stop();
    });

    // Start workers
    Timer timer;
    timer.start();

    for (std::size_t i = 0; i < config.num_connections; ++i) {
      if (config.mode == "throughput") {
        asio::co_spawn(io_context, throughput_worker(config, i),
                       asio::detached);
      } else if (config.mode == "latency") {
        asio::co_spawn(io_context, latency_worker(config, i), asio::detached);
      }
    }

    // Run threads
    std::vector<std::thread> threads;
    threads.reserve(config.num_threads - 1);
    for (std::size_t i = 1; i < config.num_threads; ++i) {
      threads.emplace_back([&io_context]() { io_context.run(); });
    }
    io_context.run();

    for (auto &t : threads) {
      t.join();
    }

    timer.stop();
    double duration = timer.elapsed_seconds();

    // Print results
    if (config.mode == "throughput") {
      print_throughput_result(g_stats, duration);
    } else if (config.mode == "latency") {
      print_latency_result(g_stats, duration);
    }

  } catch (std::exception &e) {
    std::fprintf(stderr, "Exception: %s\n", e.what());
    return 1;
  }

  return 0;
}
