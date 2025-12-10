/*
 *    tcp_relay.cpp:
 *
 *    Copyright (C) 2023-2025 Light Lin <lxrite@gmail.com> All Rights Reserved.
 *
 */

#include <asio/as_tuple.hpp>
#include <asio/bind_cancellation_slot.hpp>
#include <asio/detached.hpp>
#include <asio/experimental/awaitable_operators.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read_until.hpp>
#include <asio/signal_set.hpp>
#include <asio/steady_timer.hpp>
#include <chrono>
#include <iostream>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <vector>
#include <format>

using namespace asio::experimental::awaitable_operators;
using AddressType = std::pair<std::string, asio::ip::port_type>;

constexpr char kAppVersionString[] = "1.0.1";

constexpr std::uint32_t kResolveTimeout = 20;
constexpr std::uint32_t kConnectTimeout = 20;
constexpr std::uint32_t kHttpProxyHandshakeTimeout = 20;

enum class ViaType {
  none,
  http_proxy,
};

class Watchdog {
public:
  Watchdog(const asio::any_io_executor &executor) : timer_(executor) {}

  virtual void expires_after(const std::chrono::seconds &interval) {
    is_expired_ = false;
    timer_.expires_after(interval);
    timer_.async_wait([this](auto ec) {
      if (!ec) {
        is_expired_ = true;
        cancel_.emit(asio::cancellation_type::terminal);
      }
    });
  }

  bool is_expired() const { return is_expired_; }

  asio::cancellation_slot cancel_slot() noexcept { return cancel_.slot(); }

private:
  asio::steady_timer timer_;
  asio::cancellation_signal cancel_;
  bool is_expired_ = false;
};

class Deadline {
public:
  Deadline() = default;

  void expires_after(const std::chrono::seconds &interval) {
    deadline_ = std::chrono::steady_clock::now() + interval;
  }

  std::chrono::steady_clock::time_point time_point() const { return deadline_; }

  bool is_expired() const {
    return std::chrono::steady_clock::now() >= deadline_;
  }

private:
  std::chrono::steady_clock::time_point deadline_;
};

enum class LogLevel {
  trace = 0,
  debug = 1,
  info = 2,
  warn = 3,
  error = 4,
  disable = 5,
};

class Log {
public:
  template <typename... Args>
  static void trace(std::format_string<Args...> fmt, Args &&...args) {
    log(LogLevel::trace, fmt, std::forward<Args>(args)...);
  }

  template <typename... Args>
  static void debug(std::format_string<Args...> fmt, Args &&...args) {
    log(LogLevel::debug, fmt, std::forward<Args>(args)...);
  }

  template <typename... Args>
  static void info(std::format_string<Args...> fmt, Args &&...args) {
    log(LogLevel::info, fmt, std::forward<Args>(args)...);
  }

  template <typename... Args>
  static void error(std::format_string<Args...> fmt, Args &&...args) {
    log(LogLevel::error, fmt, std::forward<Args>(args)...);
  }

  static void set_log_level(LogLevel level) { s_log_level = level; }

private:
  template <typename... Args>
  static void log(LogLevel level, std::format_string<Args...> fmt,
                  Args &&...args) {
    if (level < s_log_level) {
      return;
    }
    std::string level_str = log_level_name(level);
    std::string message = std::format(fmt, std::forward<Args>(args)...);
    auto time = std::chrono::zoned_time{std::chrono::current_zone(),
                                        std::chrono::system_clock::now()};
    std::lock_guard<std::mutex> lock(s_log_mutex);
    std::cout << std::format("[{}] {:%F %T %Z} | {}\n", level_str, time,
                             message);
  }

  static std::string log_level_name(LogLevel level) {
    switch (level) {
    case LogLevel::trace:
      return "TRACE";
    case LogLevel::debug:
      return "DEBUG";
    case LogLevel::info:
      return "INFO ";
    case LogLevel::warn:
      return "WARN ";
    case LogLevel::error:
      return "ERROR";
    default:
      // unreachable
      std::abort();
    }
  }

  static LogLevel s_log_level;
  static std::mutex s_log_mutex;
};

LogLevel Log::s_log_level = LogLevel::info;
std::mutex Log::s_log_mutex;

enum class TransferType {
  uplink,
  downlink,
};

struct RelayConnectionOptions {
  AddressType target_address;
  std::uint32_t timeout;
  ViaType via_type;
  AddressType http_proxy_address;
};

class RelayConnection {
public:
  RelayConnection(std::uint64_t session_id,
                  const RelayConnectionOptions &options)
      : session_id_(session_id), options_(options) {}

  asio::awaitable<void> relay(asio::ip::tcp::socket client) {
    Log::info("[session: {}] | start connection from {}", session_id_,
              endpoint_to_string(client.remote_endpoint()));
    try {
      auto executor = co_await asio::this_coro::executor;
      asio::ip::tcp::socket server = co_await connect_to_server();
      if (options_.via_type == ViaType::http_proxy) {
        co_await http_proxy_handshake(server);
      }
      co_await tunnel_transfer(client, server);
    } catch (std::exception &e) {
    }
    Log::info("[session: {}] | end connection", session_id_);
  }

private:
  asio::awaitable<asio::ip::tcp::socket> connect_to_server() {
    auto address = server_address();
    const auto &host = std::get<0>(address);
    const auto &port = std::get<1>(address);
    if (options_.via_type == ViaType::http_proxy) {
      Log::debug(
          "[session: {}] | start connecting to the http proxy server {}:{}",
          session_id_, host, port);
    } else {
      Log::debug("[session: {}] | start connecting to {}:{}", session_id_, host,
                 port);
    }
    auto executor = co_await asio::this_coro::executor;
    asio::ip::tcp::resolver resolver(executor);
    Watchdog watchdog(executor);
    watchdog.expires_after(std::chrono::seconds(kResolveTimeout));
    Log::trace("[session: {}] | start resolving {}:{}", session_id_, host,
               port);
    auto [ec, resolver_entries] = co_await resolver.async_resolve(
        host, std::to_string(port),
        asio::as_tuple(asio::bind_cancellation_slot(watchdog.cancel_slot(),
                                                    asio::use_awaitable)));
    if (watchdog.is_expired()) {
      Log::error("[session: {}] | resolve {}:{} timeout", session_id_, host,
                 port);
      throw std::system_error(std::make_error_code(std::errc::timed_out));
    }
    if (ec) {
      Log::error("[session: {}] | resolve {}:{} error: {}", session_id_, host,
                 port, ec.message());
      throw std::system_error(ec);
    }
    Log::trace("[session: {}] | resolve {}:{} success", session_id_, host,
               port);
    asio::ip::tcp::socket server(executor);
    for (const auto resolver_entry : resolver_entries) {
      watchdog.expires_after(std::chrono::seconds(kConnectTimeout));
      Log::trace("[session: {}] | start connecting {}:{}({})", session_id_,
                 host, port, endpoint_to_string(resolver_entry.endpoint()));
      auto [ec] = co_await server.async_connect(
          resolver_entry, asio::as_tuple(asio::bind_cancellation_slot(
                              watchdog.cancel_slot(), asio::use_awaitable)));
      if (ec) {
        Log::trace("[session: {}] | connecte to {}:{}({}) error: {}",
                   session_id_, host, port,
                   endpoint_to_string(resolver_entry.endpoint()), ec.message());
      } else {
        Log::debug("[session: {}] | successfully connected to {}:{}({})",
                   session_id_, host, port,
                   endpoint_to_string(resolver_entry.endpoint()));
        co_return server;
      }
    }
    Log::error("[session: {}] | failed to connect to {}:{}", session_id_, host,
               port);
    throw std::runtime_error(
        std::format("failed to connect to {}:{}", session_id_, host, port));
  }

  asio::awaitable<void> http_proxy_handshake(asio::ip::tcp::socket &server) {
    const auto &target_address = options_.target_address;
    std::string http_host;
    if (std::get<0>(target_address).find(':') != std::string::npos) {
      http_host = std::format("[{}]:{}", std::get<0>(target_address),
                              std::get<1>(target_address));
    } else {
      http_host = std::format("{}:{}", std::get<0>(target_address),
                              std::get<1>(target_address));
    }
    Log::debug("[session: {}] | http-proxy handshake CONNECT {} HTTP/1.1",
               session_id_, http_host);
    std::string request_header =
        std::format("CONNECT {} HTTP/1.1\r\nHost: {}\r\nProxy-Connection: "
                    "keep-alive\r\n\r\n",
                    http_host, http_host);
    std::size_t request_header_size = request_header.size();
    std::size_t bytes_written = 0;
    auto executor = co_await asio::this_coro::executor;
    Watchdog watchdog(executor);
    while (bytes_written < request_header_size) {
      watchdog.expires_after(std::chrono::seconds(kHttpProxyHandshakeTimeout));
      auto [ec, bytes_transfered] = co_await server.async_write_some(
          asio::buffer(request_header.data() + bytes_written,
                       request_header_size - bytes_written),
          asio::as_tuple(asio::bind_cancellation_slot(watchdog.cancel_slot(),
                                                      asio::use_awaitable)));
      if (watchdog.is_expired()) {
        Log::error(
            "[session: {}] | http-proxy handshake write request header timeout",
            session_id_);
        throw std::system_error(std::make_error_code(std::errc::timed_out));
      }
      if (ec) {
        Log::error("[session: {}] | http-proxy handshake write request header "
                   "error: {}",
                   session_id_, ec.message());
        throw std::system_error(ec);
      }
      bytes_written += bytes_transfered;
    }
    std::string response_header;
    watchdog.expires_after(std::chrono::seconds(kHttpProxyHandshakeTimeout));
    auto [ec, bytes_read] = co_await asio::async_read_until(
        server, asio::dynamic_buffer(response_header, 2048), "\r\n\r\n",
        asio::as_tuple(asio::bind_cancellation_slot(watchdog.cancel_slot(),
                                                    asio::use_awaitable)));
    if (watchdog.is_expired()) {
      Log::error(
          "[session: {}] | http-proxy handshake read response header timeout",
          session_id_);
      throw std::system_error(std::make_error_code(std::errc::timed_out));
    }
    if (ec) {
      Log::error(
          "[session: {}] | http-proxy handshake read response header error: {}",
          session_id_, ec.message());
      throw std::system_error(ec);
    }

    auto first_line_end = response_header.find("\r\n");
    std::regex re("^HTTP/1\\.[01]\\s+(\\d+)\\s+.*",
                  std::regex_constants::ECMAScript |
                      std::regex_constants::icase);
    std::smatch m;
    if (!std::regex_match(response_header.cbegin(),
                          response_header.cbegin() + first_line_end, m, re)) {
      Log::error("[session: {}] | http-proxy handshake failed bad HTTP "
                 "response header",
                 session_id_);
      throw std::runtime_error("bad HTTP response header");
    }
    std::string status_code = m[1].str();
    if (status_code != "200") {
      Log::error("[session: {}] | http-proxy handshake failed response "
                 "status_code: {}",
                 session_id_, status_code);
      throw std::runtime_error("HTTP connect failed");
    }
    Log::debug("[session: {}] | http-proxy handshake success", session_id_);
  }

  asio::awaitable<void> tunnel_transfer(asio::ip::tcp::socket &client,
                                        asio::ip::tcp::socket &server) {
    Deadline deadline;
    Log::debug("[session: {}] | start tunnel transfer", session_id_);
    auto transfer_result =
        co_await (tunnel_transfer(client, server, deadline) ||
                  tunnel_transfer_timeout(deadline));
    if (transfer_result.index() == 1) {
      Log::debug(
          "[session: {}] | tunnel transfer connection closed due to timeout",
          session_id_);
    }
    Log::debug("[session: {}] | end tunnel transfer", session_id_);
  }

  asio::awaitable<void> tunnel_transfer(asio::ip::tcp::socket &client,
                                        asio::ip::tcp::socket &server,
                                        Deadline &deadline) {
    try {
      co_await (transfer(TransferType::uplink, client, server, deadline) &&
                transfer(TransferType::downlink, server, client, deadline));
    } catch (std::exception &) {
    }
  }

  asio::awaitable<void> transfer(TransferType type, asio::ip::tcp::socket &from,
                                 asio::ip::tcp::socket &to,
                                 Deadline &deadline) {
    std::array<char, 4096> buffer;
    std::string transfer_type_string = transfer_type_to_string(type);
    for (;;) {
      deadline.expires_after(std::chrono::seconds(options_.timeout));
      auto [read_error, bytes_read] = co_await from.async_read_some(
          asio::buffer(buffer), asio::as_tuple(asio::use_awaitable));
      if (read_error) {
        if (read_error.value() == asio::error::eof) {
          Log::debug("[session: {}] | {} transfer read eof", session_id_,
                     transfer_type_string);
          co_return;
        }
        Log::debug("[session: {}] | {} transfer read error: {}", session_id_,
                   transfer_type_string, read_error.message());
        throw std::system_error(read_error);
      }
      std::size_t bytes_written = 0;
      while (bytes_written < bytes_read) {
        deadline.expires_after(std::chrono::seconds(options_.timeout));
        auto [write_error, bytes_transferred] = co_await to.async_write_some(
            asio::buffer(buffer.data() + bytes_written,
                         bytes_read - bytes_written),
            asio::as_tuple(asio::use_awaitable));
        if (write_error) {
          Log::debug("[session: {}] | {} transfer write error: {}", session_id_,
                     transfer_type_string, write_error.message());
          throw std::system_error(write_error);
        }
        bytes_written += bytes_transferred;
      }
    }
  }

  asio::awaitable<void> tunnel_transfer_timeout(const Deadline &deadline) {
    asio::steady_timer timer(co_await asio::this_coro::executor);
    while (!deadline.is_expired()) {
      timer.expires_at(deadline.time_point());
      co_await timer.async_wait(asio::use_awaitable);
    }
  }

  const AddressType &server_address() const {
    switch (options_.via_type) {
    case ViaType::http_proxy:
      return options_.http_proxy_address;
    default:
      return options_.target_address;
    }
  }

  std::string endpoint_to_string(const asio::ip::tcp::endpoint &endpoint) {
    auto address = endpoint.address();
    if (address.is_v6()) {
      return std::format("[{}]:{}", address.to_string(), endpoint.port());
    } else {
      return std::format("{}:{}", address.to_string(), endpoint.port());
    }
  }

  std::string transfer_type_to_string(TransferType transfer_type) {
    switch (transfer_type) {
    case TransferType::uplink:
      return "uplink";
    case TransferType::downlink:
      return "downlink";
    default:
      // unreachable
      std::abort();
    }
  }

private:
  std::uint64_t session_id_;
  RelayConnectionOptions options_;
};

struct RelayServerOptions {
  asio::ip::address listen_address;
  asio::ip::port_type listen_port;
  AddressType target_address;
  std::uint32_t timeout;
  ViaType via_type;
  AddressType http_proxy_address;
};

class RelayServer {
public:
  RelayServer(const asio::any_io_executor &executor,
              const RelayServerOptions &options)
      : acceptor_(executor, {options.listen_address, options.listen_port}),
        options_(options) {}

  asio::awaitable<void> listen() {
    auto executor = co_await asio::this_coro::executor;
    RelayConnectionOptions conn_options = {
        .target_address = options_.target_address,
        .timeout = options_.timeout,
        .via_type = options_.via_type,
        .http_proxy_address = options_.http_proxy_address,
    };
    for (std::uint64_t session_id = 10000;; ++session_id) {
      auto client = co_await acceptor_.async_accept(asio::use_awaitable);
      asio::co_spawn(
          executor,
          [session_id, conn_options,
           client = std::move(client)]() mutable -> asio::awaitable<void> {
            RelayConnection conn(session_id, conn_options);
            co_await conn.relay(std::move(client));
          },
          asio::detached);
    }
  }

private:
  asio::ip::tcp::acceptor acceptor_;
  RelayServerOptions options_;
};

struct Args {
  asio::ip::address listen_address = asio::ip::address_v4::any();
  asio::ip::port_type listen_port = 8886;
  AddressType target_address = {"", 0};
  std::uint32_t timeout = 240;
  ViaType via_type = ViaType::none;
  AddressType http_proxy_address = {"", 0};
  LogLevel log_level = LogLevel::info;
  std::uint32_t num_threads = 4;

  static void print_usage() {
#ifdef _WIN32
    const char *prog = "tcp-relay.exe";
#else
    const char *prog = "tcp-relay";
#endif
    Args args;
    std::cout
        << "Usage: " << prog << " [options]\n\n"
        << "options:\n"
        << "  -h, --help                  Show this help message and exit\n"
        << "  -v, --version               Print the program version and exit\n"
        << "  -l, --listen_addr string    Local address to listen on (default: "
        << args.listen_address.to_string() << ")\n"
        << "  -p, --port number           Local port to listen on (default: "
        << args.listen_port << ")\n"
        << "  -t, --target string         Taget address (host:port) to "
           "connect\n"
        << "  --timeout number            Connection timeout (in seconds) "
           "(default: "
        << args.timeout << ")\n"
        << "  --via [none | http_proxy]   Transfer via other proxy (default: "
           "none)\n"
        << "  --http_proxy string         HTTP-Proxy address (host:port)\n"
        << "  --log_level string [trace | debug | info | warn | error | "
           "disable] Log level (default: info)\n"
        << "  --threads number            Number of worker threads (default: "
        << args.num_threads << ")\n";
  }

  static asio::ip::port_type parse_port(const std::string &port) {
    auto result = std::stoul(port);
    if (result <= 0 || result >= 65536) {
      throw std::invalid_argument("invalid port value");
    }
    return static_cast<asio::ip::port_type>(result);
  }

  static AddressType parse_host_port_pair(const std::string &address) {
    std::regex re(R"((.+):(\d+))");
    std::smatch m;
    if (!std::regex_match(address, m, re)) {
      throw std::invalid_argument("Invalid address");
    }
    auto port = parse_port(m[2].str());
    auto host = m[1].str();
    if (host.find(":") != std::string::npos) {
      // IPv6
      if (host[0] != '[' || host[host.size() - 1] != ']') {
        throw std::invalid_argument("Invalid address");
      }
      host = host.substr(1, host.size() - 2);
    }
    return {host, port};
  }

  static Args parse_args(const std::vector<std::string> &argv) {
    Args args;
    std::string arg;
    bool invalid_param = false;
    for (std::size_t i = 1; i < argv.size(); ++i) {
      arg = argv[i];
      if (arg == "-h" || arg == "--help") {
        print_usage();
        std::exit(EXIT_SUCCESS);
      } else if (arg == "-v" || arg == "--version") {
        std::cout << "Version: " << kAppVersionString << std::endl;
        std::exit(EXIT_SUCCESS);
      } else if (arg == "-l" || arg == "--listen_addr") {
        if (++i >= argv.size()) {
          invalid_param = true;
          break;
        }
        try {
          args.listen_address = asio::ip::make_address(argv[i]);
        } catch (std::exception &) {
          invalid_param = true;
          break;
        }
      } else if (arg == "-p" || arg == "--port") {
        if (++i >= argv.size()) {
          invalid_param = true;
          break;
        }
        try {
          args.listen_port = parse_port(argv[i]);
        } catch (std::exception &) {
          invalid_param = true;
          break;
        }
      } else if (arg == "-t" || arg == "--target") {
        if (++i >= argv.size()) {
          invalid_param = true;
          break;
        }
        try {
          args.target_address = parse_host_port_pair(argv[i]);
        } catch (std::exception &) {
          invalid_param = true;
          break;
        }
      } else if (arg == "--timeout") {
        if (++i >= argv.size()) {
          invalid_param = true;
          break;
        }
        try {
          args.timeout = std::stoul(argv[i]);
          if (args.timeout == 0) {
            invalid_param = true;
          }
        } catch (std::exception &) {
          invalid_param = true;
        }
      } else if (arg == "--via") {
        if (++i >= argv.size()) {
          invalid_param = true;
          break;
        }
        if (argv[i] == "none") {
          args.via_type = ViaType::none;
        } else if (argv[i] == "http_proxy") {
          args.via_type = ViaType::http_proxy;
        } else {
          invalid_param = true;
          break;
        }
      } else if (arg == "--http_proxy") {
        if (++i >= argv.size()) {
          invalid_param = true;
          break;
        }
        try {
          args.http_proxy_address = parse_host_port_pair(argv[i]);
        } catch (std::exception &) {
          invalid_param = true;
          break;
        }
      } else if (arg == "--log_level") {
        if (++i >= argv.size()) {
          invalid_param = true;
          break;
        }
        std::string log_level = argv[i];
        if (log_level == "trace") {
          args.log_level = LogLevel::trace;
        } else if (log_level == "debug") {
          args.log_level = LogLevel::debug;
        } else if (log_level == "info") {
          args.log_level = LogLevel::info;
        } else if (log_level == "warn") {
          args.log_level = LogLevel::warn;
        } else if (log_level == "error") {
          args.log_level = LogLevel::error;
        } else if (log_level == "disable") {
          args.log_level = LogLevel::disable;
        } else {
          invalid_param = true;
          break;
        }
      } else if (arg == "--threads") {
        if (++i >= argv.size()) {
          invalid_param = true;
          break;
        }
        try {
          args.num_threads = std::stoul(argv[i]);
          if (args.num_threads == 0) {
            invalid_param = true;
          }
        } catch (std::exception &) {
          invalid_param = true;
        }
      } else {
        std::cerr << "Unknown argument: " << arg << std::endl;
        print_usage();
        std::exit(EXIT_FAILURE);
      }
    }

    if (invalid_param) {
      std::cerr << "Invalid parameter for argument: " << arg << std::endl;
      std::exit(EXIT_FAILURE);
    }

    if (std::get<0>(args.target_address).empty() ||
        std::get<1>(args.target_address) == 0) {
      std::cerr << "Missing required argument '-t, --target'" << std::endl;
      print_usage();
      std::exit(EXIT_FAILURE);
    }

    if (args.via_type == ViaType::http_proxy) {
      if (std::get<0>(args.http_proxy_address).empty() ||
          std::get<1>(args.http_proxy_address) == 0) {
        std::cerr << "The argument '--http_proxy' is required because the "
                     "value of the argument '--via' is set to 'http_proxy'."
                  << std::endl;
        std::exit(EXIT_FAILURE);
      }
    }
    return args;
  }

  static Args parse_args(int argc, char **argv) {
    std::vector<std::string> argv_vec;
    argv_vec.reserve(argc);
    for (std::size_t i = 0; i < argc; ++i) {
      argv_vec.emplace_back(argv[i]);
    }
    return parse_args(argv_vec);
  }

  static void print_args(const Args &args) {
    if (args.listen_address.is_v6()) {
      std::cout << "Listen address: [" << args.listen_address.to_string()
                << "]:" << args.listen_port << "\n";
    } else {
      std::cout << "Listen address: " << args.listen_address.to_string() << ":"
                << args.listen_port << "\n";
    }
    std::cout << "Target address: " << std::get<0>(args.target_address) << ":"
              << std::get<1>(args.target_address) << "\n";
    if (args.via_type == ViaType::http_proxy) {
      std::cout << "Via HTTP-Proxy: " << std::get<0>(args.http_proxy_address)
                << ":" << std::get<1>(args.http_proxy_address) << "\n";
    }
    std::cout << "Connection timeout: " << args.timeout << "\n";
    std::cout << "Worker threads: " << args.num_threads << "\n";
  }
};

int main(int argc, char **argv) {
  auto args = Args::parse_args(argc, argv);
  Args::print_args(args);
  Log::set_log_level(args.log_level);
  try {
    asio::io_context io_context(args.num_threads);
    asio::signal_set signals(io_context, SIGINT, SIGTERM);
    signals.async_wait([&](auto, auto) { io_context.stop(); });
    asio::co_spawn(
        io_context,
        [args]() -> asio::awaitable<void> {
          RelayServerOptions options = {
              .listen_address = args.listen_address,
              .listen_port = args.listen_port,
              .target_address = args.target_address,
              .timeout = args.timeout,
              .via_type = args.via_type,
              .http_proxy_address = args.http_proxy_address,
          };
          RelayServer server(co_await asio::this_coro::executor, options);
          co_await server.listen();
        },
        asio::detached);
    std::vector<std::thread> threads;
    threads.reserve(args.num_threads - 1);
    for (std::uint32_t i = 1; i < args.num_threads; ++i) {
      threads.emplace_back([&io_context]() { io_context.run(); });
    }
    io_context.run();
    for (auto &t : threads) {
      t.join();
    }
  } catch (std::exception &e) {
    std::printf("Exception: %s\n", e.what());
  }
  return 0;
}
