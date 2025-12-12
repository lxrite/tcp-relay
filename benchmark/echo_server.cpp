/*
 *    echo_server.cpp:
 *
 *    A simple echo server for benchmarking tcp-relay.
 *
 *    Copyright (C) 2023-2025 Light Lin <lxrite@gmail.com> All Rights Reserved.
 *
 */

#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/signal_set.hpp>
#include <asio/use_awaitable.hpp>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

struct EchoServerConfig {
  std::uint16_t port = 5001;
  std::uint32_t num_threads = 4;
};

std::atomic<std::uint64_t> g_total_connections{0};
std::atomic<std::uint64_t> g_total_bytes{0};

asio::awaitable<void> echo_session(asio::ip::tcp::socket socket) {
  g_total_connections.fetch_add(1, std::memory_order_relaxed);
  std::array<char, 8192> buffer;
  try {
    for (;;) {
      auto [read_ec, bytes_read] = co_await socket.async_read_some(
          asio::buffer(buffer), asio::as_tuple(asio::use_awaitable));
      if (read_ec) {
        break;
      }
      g_total_bytes.fetch_add(bytes_read, std::memory_order_relaxed);

      std::size_t bytes_written = 0;
      while (bytes_written < bytes_read) {
        auto [write_ec, n] = co_await socket.async_write_some(
            asio::buffer(buffer.data() + bytes_written,
                         bytes_read - bytes_written),
            asio::as_tuple(asio::use_awaitable));
        if (write_ec) {
          co_return;
        }
        bytes_written += n;
        g_total_bytes.fetch_add(n, std::memory_order_relaxed);
      }
    }
  } catch (...) {
  }
}

asio::awaitable<void> echo_listener(asio::ip::tcp::acceptor &acceptor) {
  auto executor = co_await asio::this_coro::executor;
  for (;;) {
    auto socket = co_await acceptor.async_accept(asio::use_awaitable);
    asio::co_spawn(executor, echo_session(std::move(socket)), asio::detached);
  }
}

void print_usage() {
#ifdef _WIN32
  const char *prog = "echo-server.exe";
#else
  const char *prog = "echo-server";
#endif
  std::cout << "Usage: " << prog << " [options]\n\n"
            << "A simple TCP echo server for benchmarking.\n\n"
            << "options:\n"
            << "  -h, --help              Show this help message and exit\n"
            << "  -p, --port number       Port to listen on (default: 5001)\n"
            << "  --threads number        Number of worker threads (default: "
               "4)\n";
}

EchoServerConfig parse_args(int argc, char **argv) {
  EchoServerConfig config;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      print_usage();
      std::exit(EXIT_SUCCESS);
    } else if (arg == "-p" || arg == "--port") {
      if (++i >= argc) {
        std::cerr << "Missing value for " << arg << std::endl;
        std::exit(EXIT_FAILURE);
      }
      config.port = static_cast<std::uint16_t>(std::stoul(argv[i]));
    } else if (arg == "--threads") {
      if (++i >= argc) {
        std::cerr << "Missing value for " << arg << std::endl;
        std::exit(EXIT_FAILURE);
      }
      config.num_threads = static_cast<std::uint32_t>(std::stoul(argv[i]));
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

  std::printf("Echo Server starting on port %u with %u threads...\n",
              config.port, config.num_threads);

  try {
    asio::io_context io_context(config.num_threads);

    asio::ip::tcp::acceptor acceptor(
        io_context, {asio::ip::address_v4::any(), config.port});

    asio::signal_set signals(io_context, SIGINT, SIGTERM);
    signals.async_wait([&](auto, auto) {
      std::printf("\nShutting down...\n");
      std::printf("Total connections: %llu\n",
                  g_total_connections.load(std::memory_order_relaxed));
      std::printf("Total bytes: %llu\n",
                  g_total_bytes.load(std::memory_order_relaxed));
      io_context.stop();
    });

    asio::co_spawn(io_context, echo_listener(acceptor), asio::detached);

    std::vector<std::thread> threads;
    threads.reserve(config.num_threads - 1);
    for (std::uint32_t i = 1; i < config.num_threads; ++i) {
      threads.emplace_back([&io_context]() { io_context.run(); });
    }

    std::printf("Echo Server listening on 0.0.0.0:%u\n", config.port);

    io_context.run();

    for (auto &t : threads) {
      t.join();
    }
  } catch (std::exception &e) {
    std::fprintf(stderr, "Exception: %s\n", e.what());
    return 1;
  }

  return 0;
}
