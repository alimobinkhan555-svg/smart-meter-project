/*
 * collector.h - pthread based acquisition pipeline.
 *
 * Three stages, each on its own thread so that a slow disk or a slow
 * analytics pass can never stall pulse acquisition:
 *
 *   CollectorThread   -> reads RawSample from the PulseSource, converts the
 *                       monotonic driver timestamp into wall-clock time and
 *                       pushes it onto a bounded queue.
 *   LoggerThread      -> drains the queue into the CSV file.
 *   main()            -> drains a results queue into AnalyticsEngine and
 *                       renders the dashboard.
 *
 * The bounded queue is what gives us back-pressure: if the disk stalls,
 * the collector blocks once the queue is full instead of losing pulses.
 */
#ifndef SMART_METER_COLLECTOR_H
#define SMART_METER_COLLECTOR_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "file_io.h"
#include "pulse_source.h"

namespace smartmeter {

/* Bounded MPSC queue guarded by a mutex + two condition variables. */
template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t capacity = 1024)
        : capacity_(capacity == 0 ? 1 : capacity) {}

    /* Blocks while full. Returns false if `stop` became true. */
    bool push(T value, const std::atomic<bool>* stop) {
        std::unique_lock<std::mutex> lock(mutex_);
        not_full_.wait(lock, [&] { return queue_.size() < capacity_ || stop->load(); });
        if (stop->load()) return false;
        queue_.push_back(std::move(value));
        lock.unlock();
        not_empty_.notify_one();
        return true;
    }

    /* Non-blocking pop. */
    bool tryPop(T* out) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return false;
        *out = std::move(queue_.front());
        queue_.pop_front();
        not_full_.notify_one();
        return true;
    }

    /* Blocks for at most `timeout_ms` waiting for an element. */
    bool popFor(T* out, int timeout_ms, const std::atomic<bool>* stop) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!not_empty_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                 [&] { return !queue_.empty() || stop->load(); })) {
            return false;
        }
        if (queue_.empty()) return false;
        *out = std::move(queue_.front());
        queue_.pop_front();
        lock.unlock();
        not_full_.notify_one();
        return true;
    }

    void wakeAll() {
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    std::size_t capacity() const { return capacity_; }
    std::uint64_t dropped() const { return dropped_.load(); }

private:
    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    std::deque<T> queue_;
    std::size_t capacity_;
    std::atomic<std::uint64_t> dropped_{0};
};

/* Acquisition thread. */
class Collector {
public:
    Collector(PulseSource& source, BoundedQueue<RawSample>* output,
              std::string log_path = "");
    ~Collector();

    Collector(const Collector&) = delete;
    Collector& operator=(const Collector&) = delete;

    /* `interval_ms` is the poll timeout, i.e. the dashboard refresh rate. */
    bool start(int interval_ms, std::string* error = nullptr);
    void stop();
    bool running() const { return running_.load(); }

    /* Requests the loop to skip polling for one iteration (pause/resume). */
    void setPaused(bool paused) { paused_.store(paused); }
    bool paused() const { return paused_.load(); }

    std::uint64_t samplesCollected() const { return collected_.load(); }
    std::uint64_t errorCount() const { return errors_.load(); }
    const std::string& lastError() const;
    const std::string& logPath() const { return log_path_; }

private:
    void threadMain(int interval_ms);

    PulseSource& source_;
    BoundedQueue<RawSample>* output_;
    std::string log_path_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    std::atomic<bool> paused_{false};
    std::atomic<std::uint64_t> collected_{0};
    std::atomic<std::uint64_t> errors_{0};
    mutable std::mutex error_mutex_;
    std::string last_error_;
};

/* CSV writer thread: converts raw snapshots into Readings (delta + energy)
 * and writes them out.  Results are handed back through `output`. */
class LoggerWorker {
public:
    LoggerWorker(BoundedQueue<RawSample>* input, CsvLogger* logger,
                 BoundedQueue<Reading>* output, double pulses_per_wh = 1.0);
    ~LoggerWorker();

    LoggerWorker(const LoggerWorker&) = delete;
    LoggerWorker& operator=(const LoggerWorker&) = delete;

    bool start(std::string* error = nullptr);
    void stop();
    bool running() const { return running_.load(); }
    std::uint64_t rowsWritten() const { return rows_.load(); }
    std::uint64_t writeErrors() const { return errors_.load(); }
    std::uint64_t totalPulses() const { return total_pulses_.load(); }

private:
    void threadMain();

    BoundedQueue<RawSample>* input_;
    CsvLogger* logger_;
    BoundedQueue<Reading>* output_;
    double pulses_per_wh_{1.0};
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    std::atomic<std::uint64_t> rows_{0};
    std::atomic<std::uint64_t> errors_{0};
    std::atomic<std::uint64_t> total_pulses_{0};
    std::uint64_t previous_pulses_{0};
};

}  // namespace smartmeter

#endif /* SMART_METER_COLLECTOR_H */
