#include "cxxopts.hpp"
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <mutex>
#include <numa.h>
#include <random>
#include <spdlog/spdlog.h>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <x86intrin.h>

#define DB_SIZE 77309411328ULL // 4 GB
#define LOCAL_NUMA 0

// Define the key and value types
using KeyType = uint64_t;
using ValueType = uint64_t;

struct alignas(16) KVEntry
{
    KeyType key;
    ValueType value;
};

KVEntry *kv_store = nullptr;

// Function to initialize the KV store with random keys and values
void initialize_kv_store(uint64_t num_entries, uint64_t num_threads = std::thread::hardware_concurrency())
{
    uint64_t num_init_threads = std::min(num_threads, num_entries);
    std::vector<std::thread> init_threads;
    uint64_t entries_per_thread = num_entries / num_init_threads;

    // Reserve space for the KV store on LOCAL NUMA node
    kv_store = reinterpret_cast<KVEntry *>(numa_alloc_onnode(num_entries * sizeof(KVEntry), LOCAL_NUMA));
    if (!kv_store)
    {
        spdlog::error("Failed to allocate memory for KV store");
        return;
    }

    for (uint64_t t = 0; t < num_init_threads; ++t)
    {
        init_threads.emplace_back([&, t]() {
            std::mt19937_64 local_rng(42 + t); // Different seed for each thread
            std::uniform_int_distribution<KeyType> local_dist;

            uint64_t start = t * entries_per_thread;
            uint64_t end = (t == num_init_threads - 1) ? num_entries : (t + 1) * entries_per_thread;

            for (uint64_t i = start; i < end; ++i)
            {
                KeyType key = local_dist(local_rng);
                ValueType value = local_rng(); // Random value
                kv_store[i] = {key, value};
            }
        });
    }

    for (auto &thread : init_threads)
    {
        thread.join();
    }
}

// Zipfian generator
class ZipfianGenerator
{
    double theta;
    uint64_t n;
    double zetan;
    double alpha;
    double eta;
    std::mt19937_64 rng;
    std::uniform_real_distribution<double> dist;

    static double zeta(uint64_t n, double theta)
    {
        double sum = 0.0;
        for (uint64_t i = 1; i <= n; ++i)
        {
            sum += 1.0 / pow(i, theta);
        }
        return sum;
    }

  public:
    ZipfianGenerator(uint64_t n, double theta = 0.99) : theta(theta), n(n), rng(std::random_device{}()), dist(0.0, 1.0)
    {
        zetan = zeta(n, theta);
        alpha = 1.0 / (1.0 - theta);
        eta = (1 - pow(2.0 / static_cast<double>(n), 1 - theta)) / (1 - zeta(2, theta) / zetan);
    }

    KeyType next()
    {
        double u = dist(rng);
        double uz = u * zetan;
        if (uz < 1.0)
            return 1;
        if (uz < 1.0 + pow(0.5, theta))
            return 2;
        return static_cast<KeyType>(1 + static_cast<KeyType>(n * pow(eta * u - eta + 1, alpha)));
    }
};

// Thread function to perform get operations with configurable distribution
void thread_worker(std::atomic<uint64_t> &completed_ops, std::atomic<uint64_t> &total_duration_ns,
                   std::chrono::nanoseconds duration, std::string dist_type, uint64_t num_entries, uint64_t *latencies,
                   uint64_t &latency_ops)
{
    std::mt19937_64 rng(std::random_device{}());
    std::uniform_int_distribution<KeyType> uniform_dist(0, num_entries - 1);
    ZipfianGenerator zipf_gen(num_entries);

    auto start_time = std::chrono::high_resolution_clock::now();
    auto end_time = start_time + duration;

    uint64_t local_ops = 0;

    if (latencies == nullptr)
    {
        while (std::chrono::high_resolution_clock::now() < end_time)
        {
            uint64_t index = (dist_type == "zipf") ? zipf_gen.next() : uniform_dist(rng);

            KeyType key = kv_store[index].key;
            ValueType value = kv_store[index].value;

            volatile ValueType sink = value;
            (void)sink;
        }

        return;
    }

    while (std::chrono::high_resolution_clock::now() < end_time)
    {
        auto access_start = __rdtsc();
        uint64_t index = (dist_type == "zipf") ? zipf_gen.next() : uniform_dist(rng);
        KeyType key = kv_store[index].key;
        ValueType value = kv_store[index].value;
        auto access_end = __rdtsc();

        volatile ValueType sink = value;
        (void)sink;

        if (local_ops % 100000 == 0)
        {
            latencies[latency_ops++] = access_end - access_start; // Store latency
        }
        ++local_ops;
    }

    // Calculate latency percentiles
    // std::sort(latencies, latencies + local_ops);

    auto actual_end = std::chrono::high_resolution_clock::now();
    uint64_t thread_duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(actual_end - start_time).count();
    completed_ops += local_ops;
    total_duration_ns += thread_duration_ns;
}

// Thread function to perform Non-Temporal Load with configurable distribution
void nt_thread_worker(std::atomic<uint64_t> &completed_ops, std::atomic<uint64_t> &total_duration_ns,
                      std::chrono::nanoseconds duration, std::string dist_type, uint64_t num_entries,
                      uint64_t *latencies, uint64_t &latency_ops)
{
    std::mt19937_64 rng(std::random_device{}());
    std::uniform_int_distribution<KeyType> uniform_dist(0, num_entries - 1);
    ZipfianGenerator zipf_gen(num_entries);

    auto start_time = std::chrono::high_resolution_clock::now();
    auto end_time = start_time + duration;

    if (latencies == nullptr)
    {
        while (std::chrono::high_resolution_clock::now() < end_time)
        {
            uint64_t index = (dist_type == "zipf") ? zipf_gen.next() : uniform_dist(rng);
            __m128i *ptr = reinterpret_cast<__m128i *>(&kv_store[index]);

            // Streaming load
            __m128i result = _mm_stream_load_si128(ptr);
            uint64_t val;
            _mm_storel_epi64(reinterpret_cast<__m128i *>(&val), result); // Only store lower 64 bits (value)

            volatile ValueType sink = val;
            (void)sink;
        }

        return;
    }

    uint64_t local_ops = 0;
    while (std::chrono::high_resolution_clock::now() < end_time)
    {
        uint64_t index = (dist_type == "zipf") ? zipf_gen.next() : uniform_dist(rng);
        __m128i *ptr = reinterpret_cast<__m128i *>(&kv_store[index]);

        // Streaming load
        auto access_start = __rdtsc();
        __m128i result = _mm_stream_load_si128(ptr);
        uint64_t val;
        _mm_storel_epi64(reinterpret_cast<__m128i *>(&val), result); // Only store lower 64 bits (value)
        auto access_end = __rdtsc();

        volatile ValueType sink = val;
        (void)sink;

        if (local_ops % 100000 == 0)
        {
            latencies[latency_ops++] = access_end - access_start; // Store latency
        }
        ++local_ops;
    }

    auto actual_end = std::chrono::high_resolution_clock::now();
    uint64_t thread_duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(actual_end - start_time).count();

    completed_ops += local_ops;
    total_duration_ns += thread_duration_ns;
}

int main(int argc, char *argv[])
{
    const uint64_t total_size_bytes = DB_SIZE;
    const uint64_t entry_size = sizeof(KeyType) + sizeof(ValueType); // Approximate size per entry
    const uint64_t num_entries = total_size_bytes / entry_size;

    uint64_t num_threads = std::thread::hardware_concurrency();
    int benchmark_duration_seconds = 5;
    int warmup_duration_seconds = 1; // Default warm-up duration
    std::string dist_type = "uniform";
    bool use_non_temporal = false;

    try
    {
        cxxopts::Options options("kv_latency_benchmark", "Memory latency benchmark with configurable options");
        options.add_options()
        (   "t,threads", "Number of threads", cxxopts::value<uint64_t>()->default_value(std::to_string(num_threads)))(
            "d,duration", "Benchmark duration in seconds", cxxopts::value<int>()->default_value("5"))(
            "w,warmup", "Warm-up duration in seconds", cxxopts::value<int>()->default_value("1"))(
            "dist", "Access distribution: uniform or zipf", cxxopts::value<std::string>()->default_value("uniform"))(
            "nt", "Use non-temporal loads", cxxopts::value<bool>()->default_value("false"))("h,help", "Print usage");

        auto result = options.parse(argc, argv);

        if (result.count("help"))
        {
            std::cout << options.help() << std::endl;
            return 0;
        }

        num_threads = result["threads"].as<uint64_t>();
        benchmark_duration_seconds = result["duration"].as<int>();
        warmup_duration_seconds = result["warmup"].as<int>();
        dist_type = result["dist"].as<std::string>();
        use_non_temporal = result["nt"].as<bool>();
    }
    catch (const std::exception &e)
    {
        spdlog::error("Error parsing command line arguments: {}", e.what());
        return 1;
    }

    spdlog::info("Initializing KV store with {} entries (~{}GB) using {} threads...", num_entries,
                 total_size_bytes / (1024 * 1024 * 1024), num_threads);

    initialize_kv_store(num_entries, num_threads);
    std::cout << "Initialization complete.\n";

    std::vector<std::thread> threads;
    std::atomic<uint64_t> total_duration_ns(0);
    std::atomic<uint64_t> completed_ops(0);

    std::vector<uint64_t *> latencies(num_threads);
    std::vector<uint64_t> latency_ops(num_threads, 0);

    for (uint64_t i = 0; i < num_threads; ++i)
    {
        latencies[i] = reinterpret_cast<uint64_t *>(numa_alloc_onnode(1e4 * sizeof(uint64_t), LOCAL_NUMA));
        if (!latencies[i])
        {
            spdlog::error("Failed to allocate memory for latencies");
            return 1;
        }
        std::memset(latencies[i], 0, 1e4 * sizeof(uint64_t));
    }

    spdlog::info("Warm-up phase: running without measuring...");

    // Warm-up phase: run without measuring
    auto warmup_start = std::chrono::high_resolution_clock::now();
    if (use_non_temporal)
    {
        for (uint64_t i = 0; i < num_threads; ++i)
        {
            threads.emplace_back(nt_thread_worker, std::ref(completed_ops), std::ref(total_duration_ns),
                                 std::chrono::seconds(warmup_duration_seconds), dist_type, num_entries, nullptr, std::ref(latency_ops[i]));
        }
    }
    else
    {
        for (uint64_t i = 0; i < num_threads; ++i)
        {
            threads.emplace_back(thread_worker, std::ref(completed_ops), std::ref(total_duration_ns),
                                 std::chrono::seconds(warmup_duration_seconds), dist_type, num_entries, nullptr, std::ref(latency_ops[i]));
        }
    }

    for (auto &t : threads)
    {
        t.join();
    }
    auto warmup_end = std::chrono::high_resolution_clock::now();
    double warmup_duration = std::chrono::duration<double>(warmup_end - warmup_start).count();
    spdlog::info("Warm-up complete. Duration: {:.2f} seconds", warmup_duration);

    // Clear Cache
    spdlog::info("Clearing cache...");
    for (uint64_t i = 0; i < num_entries; i += 64)
    {
        _mm_clflush(&kv_store[i]);
    }

    // Clear threads and start the actual benchmark
    threads.clear();

    spdlog::info("Starting benchmark for {} seconds using {} distribution...", benchmark_duration_seconds, dist_type);
    auto benchmark_start = std::chrono::high_resolution_clock::now();
    if (use_non_temporal)
    {
        spdlog::info("Using non-temporal loads");
        for (uint64_t i = 0; i < num_threads; ++i)
        {
            threads.emplace_back(nt_thread_worker, std::ref(completed_ops), std::ref(total_duration_ns),
                                 std::chrono::seconds(benchmark_duration_seconds), dist_type, num_entries,
                                 latencies[i], std::ref(latency_ops[i]));
        }
    }
    else
    {
        spdlog::info("Using regular loads");
        for (uint64_t i = 0; i < num_threads; ++i)
        {
            threads.emplace_back(thread_worker, std::ref(completed_ops), std::ref(total_duration_ns),
                                 std::chrono::seconds(benchmark_duration_seconds), dist_type, num_entries,
                                 latencies[i], std::ref(latency_ops[i]));
        }
    }

    for (auto &t : threads)
    {
        t.join();
    }

    auto benchmark_end = std::chrono::high_resolution_clock::now();
    uint64_t benchmark_duration_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(benchmark_end - benchmark_start).count();

    double total_duration_with_warmup = std::chrono::duration<double>(benchmark_end - warmup_start).count();
    double actual_benchmark_duration = total_duration_with_warmup - warmup_duration;

    // Merge latency results from all threads and calculate percentiles
    std::vector<uint64_t> all_latencies;
    uint64_t total_latency_ops = 0;
    for (uint64_t i = 0; i < num_threads; ++i)
    {
        all_latencies.insert(all_latencies.end(), latencies[i], latencies[i] + latency_ops[i]);
        total_latency_ops += latency_ops[i];
    }

    spdlog::info("Benchmark complete");
    spdlog::info("Sorting latencies for percentile calculation...");

    std::sort(all_latencies.begin(), all_latencies.end());
    uint64_t latency_50th = all_latencies[total_latency_ops * 0.5];
    uint64_t latency_90th = all_latencies[total_latency_ops * 0.9];
    uint64_t latency_95th = all_latencies[total_latency_ops * 0.95];
    uint64_t latency_99th = all_latencies[total_latency_ops * 0.99];
    uint64_t latency_99_9th = all_latencies[total_latency_ops * 0.999];

    spdlog::info("Total operations: {}", completed_ops.load());
    spdlog::info("Total time (including warm-up): {:.2f} seconds", total_duration_with_warmup);
    spdlog::info("Actual benchmark time: {:.2f} seconds", actual_benchmark_duration);

    // Throughput and Latency Results
    float throughput = (static_cast<float>(completed_ops.load())) / (1e6f * actual_benchmark_duration);
    spdlog::info("Throughput: {:.2f} MOps/s", throughput);

    spdlog::info("Latency (50th percentile): {} ns", latency_50th);
    spdlog::info("Latency (90th percentile): {} ns", latency_90th);
    spdlog::info("Latency (95th percentile): {} ns", latency_95th);
    spdlog::info("Latency (99th percentile): {} ns", latency_99th);
    spdlog::info("Latency (99.9th percentile): {} ns", latency_99_9th);
    spdlog::info("Latency (max): {} ns", all_latencies.back());
    spdlog::info("Latency (min): {} ns", all_latencies.front());

    numa_free(kv_store, num_entries * sizeof(KVEntry));
    for (uint64_t i = 0; i < num_threads; ++i)
    {
        numa_free(latencies[i], 1e4 * sizeof(uint64_t));
        latencies[i] = nullptr;
    }
    kv_store = nullptr;

    return 0;
}
