#define LOCAL_NUMA 1
#include <cstring>
#include <cxxopts.hpp>
#include <immintrin.h>
#include <x86intrin.h>
#include <iostream>
#include <numa.h>
#include <numaif.h>
#include <numeric>
#include <omp.h>
#include <random>
#include <spdlog/spdlog.h>
#include <vector>

constexpr int NTIMES = 10;

uint64_t *a;
uint64_t *b;
uint64_t *c;

uint64_t* alloc_numa_aligned_f64(size_t N, int node) {
    // Align memory to 64 bytes for AVX/AVX2 operations
    void *aligned_ptr = nullptr;
    if (posix_memalign(&aligned_ptr, 64, N * sizeof(uint64_t)) != 0)
    {
        spdlog::error("Failed to allocate aligned memory");
        exit(1);
    }
    numa_tonode_memory(aligned_ptr, N * sizeof(uint64_t), LOCAL_NUMA);
    return static_cast<uint64_t *>(aligned_ptr);
}


void init_data(size_t N)
{
    if (numa_available() < 0)
    {
        spdlog::error("NUMA not available on this system");
        exit(1);
    }

    a = alloc_numa_aligned_f64(N, LOCAL_NUMA);
    b = alloc_numa_aligned_f64(N, LOCAL_NUMA);
    c = alloc_numa_aligned_f64(N, LOCAL_NUMA);

#pragma omp parallel for
    for (size_t i = 0; i < N; ++i)
    {
        a[i] = 1.0;
        b[i] = 2.0;
        c[i] = 0.0;
    }
}

std::vector<size_t> generate_hotcold_indices(size_t total, double hot_ratio)
{
    size_t hot_size = static_cast<size_t>(total * hot_ratio);
    std::vector<size_t> indices(total);
    std::iota(indices.begin(), indices.end(), 0);
    std::shuffle(indices.begin(), indices.end(), std::mt19937{std::random_device{}()});
    return indices;
}

inline uint64_t rdtsc()
{
    unsigned int lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

void run_stream_kernel(const std::vector<size_t> &indices, size_t N, bool use_nt, uint64_t duration_cycles)
{
    uint64_t scalar = 3;
    std::vector<double> times(4, 0.0);
    uint64_t start, end;

    std::function<void(uint64_t*, uint64_t*)> stream_copy = [](uint64_t *dst, uint64_t *src) { *dst = *src; };
    std::function<void(uint64_t*, uint64_t*, uint64_t)> stream_scalar = [](uint64_t *dst, uint64_t* src, uint64_t scalar) { *dst = *src * scalar; };
    std::function<void(uint64_t*, uint64_t*, uint64_t*)> stream_sum = [](uint64_t *dst, uint64_t* src1, uint64_t* src2) { *dst = *src1 + *src2; };
    std::function<void(uint64_t*, uint64_t*, uint64_t*, uint64_t)> stream_acc = [](uint64_t *dst, uint64_t* src1, uint64_t* src2, uint64_t scalar) { *dst = *src1 + (*src2 * scalar); };

    // If non-temporal stores and loads are enabled, use specialized instructions
    // Non-temporal operations bypass the CPU cache, directly writing to memory.
    if (use_nt)
    {
        stream_copy = [](uint64_t *dst, uint64_t *src) {
            // Use streaming load and store
            __m128i* ptr = reinterpret_cast<__m128i *>(src);
            __m128i val = _mm_stream_load_si128(ptr);
            _mm_storel_epi64(reinterpret_cast<__m128i*>(dst), val);
        };

        // Non-temporal store of repeated value
        stream_scalar = [](uint64_t *dst, uint64_t* src, uint64_t scalar) {
            // Use 64-bit non-temporal store with AVX-512
            __m128i* ptr = reinterpret_cast<__m128i *>(src);
            __m128i val = _mm_stream_load_si128(ptr);
            __m128i scalar_val = _mm_set1_epi64x(scalar);
            __m128i result = _mm_mullo_epi64(val, scalar_val);
            _mm_stream_si128(reinterpret_cast<__m128i*>(dst), result);
        };

        stream_sum = [](uint64_t *dst, uint64_t* src1, uint64_t* src2) {
            // Use 64-bit non-temporal store
            __m128i* ptr1 = reinterpret_cast<__m128i *>(src1);
            __m128i* ptr2 = reinterpret_cast<__m128i *>(src2);
            __m128i val1 = _mm_stream_load_si128(ptr1);
            __m128i val2 = _mm_stream_load_si128(ptr2);
            __m128i result = _mm_add_epi64(val1, val2);
            _mm_stream_si128(reinterpret_cast<__m128i*>(dst), result);
        };

        stream_acc = [](uint64_t *dst, uint64_t* src1, uint64_t* src2, uint64_t scalar) {
            // Use 64-bit non-temporal store
            __m128i* ptr1 = reinterpret_cast<__m128i *>(src1);
            __m128i* ptr2 = reinterpret_cast<__m128i *>(src2);
            __m128i val1 = _mm_stream_load_si128(ptr1);
            __m128i val2 = _mm_stream_load_si128(ptr2);
            __m128i scalar_val = _mm_set1_epi64x(scalar);
            __m128i result = _mm_add_epi64(val1, _mm_mullo_epi64(val2, scalar_val));
            _mm_stream_si128(reinterpret_cast<__m128i*>(dst), result);
        };

    }

    uint64_t cycles_elapsed = 0;
    int k = 0;
    const unsigned int num_threads = 16;
    std::vector<std::thread> threads;

    while (cycles_elapsed < duration_cycles)
    {
        start = rdtsc();
        threads.clear();
        for (unsigned int t = 0; t < num_threads; ++t) {
            threads.emplace_back([&, t]() {
                for (size_t i = t; i < N; i += num_threads) {
                    size_t idx = indices[i];
                    stream_copy(&c[idx], &a[idx]);
                }
            });
        }
        for (auto& thread : threads) thread.join();
        end = rdtsc();
        times[0] += static_cast<double>(end - start);
        cycles_elapsed += (end - start);

        if (cycles_elapsed >= duration_cycles)
            break;

        start = rdtsc();
        threads.clear();
        for (unsigned int t = 0; t < num_threads; ++t) {
            threads.emplace_back([&, t]() {
                for (size_t i = t; i < N; i += num_threads) {
                    size_t idx = indices[i];
                    stream_scalar(&b[idx], &a[idx], scalar);
                }
            });
        }
        for (auto& thread : threads) thread.join();
        end = rdtsc();
        times[1] += static_cast<double>(end - start);
        cycles_elapsed += (end - start);

        if (cycles_elapsed >= duration_cycles)
            break;

        start = rdtsc();
        threads.clear();
        for (unsigned int t = 0; t < num_threads; ++t) {
            threads.emplace_back([&, t]() {
                for (size_t i = t; i < N; i += num_threads) {
                    size_t idx = indices[i];
                    stream_sum(&c[idx], &b[idx], &a[idx]);
                }
            });
        }
        for (auto& thread : threads) thread.join();
        end = rdtsc();
        times[2] += static_cast<double>(end - start);
        cycles_elapsed += (end - start);

        if (cycles_elapsed >= duration_cycles)
            break;

        start = rdtsc();
        threads.clear();
        for (unsigned int t = 0; t < num_threads; ++t) {
            threads.emplace_back([&, t]() {
                for (size_t i = t; i < N; i += num_threads) {
                    size_t idx = indices[i];
                    stream_acc(&c[idx], &b[idx], &a[idx], scalar);
                }
            });
        }
        for (auto& thread : threads) thread.join();
        end = rdtsc();
        times[3] += static_cast<double>(end - start);
        cycles_elapsed += (end - start);

        ++k;
    }

    spdlog::info("\nAverage cycles over {} iterations:", NTIMES);
    spdlog::info("Copy:  {:.0f}", times[0] / NTIMES);
    spdlog::info("Scale: {:.0f}", times[1] / NTIMES);
    spdlog::info("Add:   {:.0f}", times[2] / NTIMES);
    spdlog::info("Triad: {:.0f}", times[3] / NTIMES);
}

// Function to estimate clock frequency
double get_cycles_per_second()
{
    uint64_t start = rdtsc();
    sleep(10); // Sleep for 1 second
    uint64_t end = rdtsc();
    return static_cast<double>(end - start) / 10.0; // Convert to cycles per second
}

int main(int argc, char **argv)
{
    cxxopts::Options options("StreamBenchmark",
                             "Modified STREAM benchmark with hot/cold access and non-temporal stores");
    options.add_options()
    ("s,size", "Array size", cxxopts::value<size_t>()->default_value("100000000"))
    ("r,hot-ratio", "Hot region ratio", cxxopts::value<double>()->default_value("0.8"))
    ("t,nt", "Use non-temporal stores")
    ("d,duration", "Duration in seconds", cxxopts::value<int>()->default_value("1"))
    ("h,help", "Print usage");

    auto result = options.parse(argc, argv);

    if (result.count("help"))
    {
        std::cout << options.help() << std::endl;
        return 0;
    }

    size_t N = result["size"].as<size_t>();
    double hot_ratio = result["hot-ratio"].as<double>();
    bool use_nt = result.count("nt") > 0;
    int duration_sec = result["duration"].as<int>();

    float cycles_per_second = get_cycles_per_second();
    uint64_t duration_cycles = static_cast<uint64_t>(duration_sec) * cycles_per_second;

    spdlog::info("Initializing arrays with N = {}", N);
    init_data(N);

    spdlog::info("Generating hot/cold index pattern with hot ratio = {:.2f}", hot_ratio);
    auto indices = generate_hotcold_indices(N, hot_ratio);

    spdlog::info("Running STREAM kernels (non-temporal: {}) for {} seconds", use_nt, duration_sec);
    run_stream_kernel(indices, N, use_nt, duration_cycles);

    numa_free(a, N * sizeof(double));
    numa_free(b, N * sizeof(double));
    numa_free(c, N * sizeof(double));

    return 0;
}
