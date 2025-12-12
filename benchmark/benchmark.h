/*
 *    benchmark.h:
 *
 *    Copyright (C) 2023-2025 Light Lin <lxrite@gmail.com> All Rights Reserved.
 *
 */

#ifndef BENCHMARK_H_
#define BENCHMARK_H_

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

struct BenchmarkConfig {
  std::string host = "127.0.0.1";
  std::uint16_t port = 8886;
  std::string mode = "throughput";
  std::size_t num_connections = 10;
  std::size_t duration_seconds = 10;
  std::size_t message_size = 4096;
  std::size_t num_threads = 4;
};

class Statistics {
public:
  void add_sample(double value) {
    samples_.push_back(value);
    sum_ += value;
  }

  void add_bytes(std::size_t bytes) {
    total_bytes_.fetch_add(bytes, std::memory_order_relaxed);
  }

  void add_connection() {
    total_connections_.fetch_add(1, std::memory_order_relaxed);
  }

  void add_error() { total_errors_.fetch_add(1, std::memory_order_relaxed); }

  std::size_t total_bytes() const {
    return total_bytes_.load(std::memory_order_relaxed);
  }

  std::size_t total_connections() const {
    return total_connections_.load(std::memory_order_relaxed);
  }

  std::size_t total_errors() const {
    return total_errors_.load(std::memory_order_relaxed);
  }

  double average() const {
    if (samples_.empty())
      return 0.0;
    return sum_ / static_cast<double>(samples_.size());
  }

  double percentile(double p) {
    if (samples_.empty())
      return 0.0;
    std::sort(samples_.begin(), samples_.end());
    std::size_t index =
        static_cast<std::size_t>(p / 100.0 * (samples_.size() - 1));
    return samples_[index];
  }

  double min_value() const {
    if (samples_.empty())
      return 0.0;
    return *std::min_element(samples_.begin(), samples_.end());
  }

  double max_value() const {
    if (samples_.empty())
      return 0.0;
    return *std::max_element(samples_.begin(), samples_.end());
  }

  std::size_t sample_count() const { return samples_.size(); }

  void merge(Statistics &other) {
    samples_.insert(samples_.end(), other.samples_.begin(),
                    other.samples_.end());
    sum_ += other.sum_;
    total_bytes_.fetch_add(other.total_bytes_.load(std::memory_order_relaxed),
                           std::memory_order_relaxed);
    total_connections_.fetch_add(
        other.total_connections_.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    total_errors_.fetch_add(other.total_errors_.load(std::memory_order_relaxed),
                            std::memory_order_relaxed);
  }

private:
  std::vector<double> samples_;
  double sum_ = 0.0;
  std::atomic<std::size_t> total_bytes_{0};
  std::atomic<std::size_t> total_connections_{0};
  std::atomic<std::size_t> total_errors_{0};
};

class Timer {
public:
  void start() { start_time_ = std::chrono::steady_clock::now(); }

  void stop() { end_time_ = std::chrono::steady_clock::now(); }

  double elapsed_seconds() const {
    auto duration = end_time_ - start_time_;
    return std::chrono::duration<double>(duration).count();
  }

  double elapsed_microseconds() const {
    auto duration = end_time_ - start_time_;
    return std::chrono::duration<double, std::micro>(duration).count();
  }

  static std::chrono::steady_clock::time_point now() {
    return std::chrono::steady_clock::now();
  }

private:
  std::chrono::steady_clock::time_point start_time_;
  std::chrono::steady_clock::time_point end_time_;
};

inline void print_throughput_result(const Statistics &stats, double duration) {
  double total_mb =
      static_cast<double>(stats.total_bytes()) / (1024.0 * 1024.0);
  double throughput_mbps = total_mb / duration;

  std::printf("\n=== Throughput Test Results ===\n");
  std::printf("Duration:        %.2f seconds\n", duration);
  std::printf("Total Data:      %.2f MB\n", total_mb);
  std::printf("Throughput:      %.2f MB/s\n", throughput_mbps);
  std::printf("Connections:     %zu\n", stats.total_connections());
  std::printf("Errors:          %zu\n", stats.total_errors());
}

inline void print_latency_result(Statistics &stats, double duration) {
  std::printf("\n=== Latency Test Results ===\n");
  std::printf("Duration:        %.2f seconds\n", duration);
  std::printf("Samples:         %zu\n", stats.sample_count());
  std::printf("Avg Latency:     %.2f us\n", stats.average());
  std::printf("Min Latency:     %.2f us\n", stats.min_value());
  std::printf("Max Latency:     %.2f us\n", stats.max_value());
  std::printf("P50 Latency:     %.2f us\n", stats.percentile(50));
  std::printf("P95 Latency:     %.2f us\n", stats.percentile(95));
  std::printf("P99 Latency:     %.2f us\n", stats.percentile(99));
  std::printf("Errors:          %zu\n", stats.total_errors());
}

inline void print_connection_result(const Statistics &stats, double duration) {
  double conn_per_sec =
      static_cast<double>(stats.total_connections()) / duration;

  std::printf("\n=== Connection Rate Test Results ===\n");
  std::printf("Duration:        %.2f seconds\n", duration);
  std::printf("Total Conns:     %zu\n", stats.total_connections());
  std::printf("Conn Rate:       %.2f conn/s\n", conn_per_sec);
  std::printf("Errors:          %zu\n", stats.total_errors());
}

#endif // BENCHMARK_H_
