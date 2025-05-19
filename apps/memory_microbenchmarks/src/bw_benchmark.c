#define _GNU_SOURCE
#include <fcntl.h>
#include <float.h>
#include <immintrin.h>
#include <limits.h>
#include <math.h>
#include <numa.h>
#include <numaif.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>
#include <x86intrin.h>


#define LOG_INTERVAL_MS 1000
#define MAX_THREADS 16

#define LOCAL_NUMA 1
#define REMOTE_NUMA 0

#define HOTNESS 0
#define ZIPF_ALPHA 1.5

// 75GB/20GB
#define WSS 77309411328ULL
#define HOTSS 21474836480ULL

#define CHUNK_SIZE 4096
#define CL_PER_CHUNK 64

size_t pg_size;

static inline __attribute__((always_inline)) unsigned long rdtsc()
{
    unsigned long a, d;

    __asm__ volatile("rdtsc" : "=a"(a), "=d"(d));

    return (a | (d << 32));
}

static inline __attribute__((always_inline)) unsigned long rdtscp()
{
    unsigned long a, d, c;

    __asm__ volatile("rdtscp" : "=a"(a), "=d"(d), "=c"(c));

    return (a | (d << 32));
}

// Fast Xorshift RNG
static inline uint64_t xorshift64(uint64_t *state)
{
    uint64_t y = *state;
    y ^= y << 13;
    y ^= y >> 7;
    y ^= y << 17;
    *state = y;
    return y;
}

// Function to generate Zipfian distributed indices
void precompute_zipf(int slots, double alpha, int *zipf_table, int table_size)
{
    double c = 0.0;
    for (int i = 1; i <= slots; i++)
    {
        c += 1.0 / pow(i, alpha);
    }
    c = 1.0 / c;

    int index = 0;
    for (int i = 1; i <= slots; i++)
    {
        int freq = (int)(c / pow(i, alpha) * slots);
        for (int j = 0; j < freq && index < table_size; j++)
        {
            zipf_table[index++] = i - 1;
        }
    }
}

// Sample from precomputed Zipfian table
static inline __attribute__((always_inline)) int zipfian_sample(int *zipf_table, int table_size, uint64_t *xorshift_state)
{
    // Use Non-Temporal Load to avoid cache pollution
    int index = xorshift64(xorshift_state) % table_size;
    return zipf_table[index];
}

typedef struct
{
    int thread_id;
    size_t buf_size;
    size_t hot_size;
    _Atomic uint64_t *count_ptr;
    int manual_placement;
    size_t local_hot_pages;
    int reset_mbind;
    size_t local_free;
    _Atomic int finish;
} ThreadArgs;

#define MAP_HUGE_1GB (30 << MAP_HUGE_SHIFT)

_Atomic int g_move_hotset;

void *thread_function(void *arg)
{
    ThreadArgs *args = (ThreadArgs *)arg;
    int mmap_flags = MAP_PRIVATE | MAP_ANONYMOUS;
    if (getenv("GUPS_HUGEPAGES") != NULL)
    {
        mmap_flags |= MAP_HUGETLB;
    }
    if (getenv("GUPS_HUGEPAGES_1GB") != NULL)
    {
        mmap_flags |= MAP_HUGETLB;
        mmap_flags |= MAP_HUGE_1GB;
    }

    // Allocate Memory
    char *a = mmap(0, args->buf_size, PROT_READ | PROT_WRITE, mmap_flags, -1, 0);

    if (a == NULL)
    {
        printf("mmap failed\n");
        return NULL;
    }

    uint64_t cur_ts = 0, prev_ts = 0;
    cur_ts = rdtscp();
    prev_ts = cur_ts;

    if (args->manual_placement)
    {
        unsigned long local_nodemask = (1UL << LOCAL_NUMA);
        unsigned long remote_nodemask = (1UL << REMOTE_NUMA);

        // New manual placement mechanism

        // Set mbind policy for hot set
        if (args->local_hot_pages > 0)
        {
            if (mbind(a + args->buf_size - args->hot_size, args->local_hot_pages * pg_size, MPOL_BIND, &local_nodemask,
                      sizeof(local_nodemask) * 8, MPOL_MF_STRICT) != 0)
            {
                fprintf(stderr, "second mbind failed\n");
                return NULL;
            }
        }
        if (mbind(a + args->buf_size - args->hot_size + args->local_hot_pages * pg_size,
                  args->hot_size - args->local_hot_pages * pg_size, MPOL_BIND, &remote_nodemask,
                  sizeof(remote_nodemask) * 8, MPOL_MF_STRICT) != 0)
        {
            fprintf(stderr, "third mbind failed\n");
            return NULL;
        }

        // Set mbind policy for cold set
        size_t cold_in_local = args->local_free - args->hot_size;
        if (cold_in_local > args->buf_size - args->hot_size)
        {
            cold_in_local = args->buf_size - args->hot_size;
        }
        // printf("fourth mbind, cold_in_local: %lu\n", cold_in_local);
        if (cold_in_local > 0 &&
            mbind(a, cold_in_local, MPOL_BIND, &local_nodemask, sizeof(local_nodemask) * 8, MPOL_MF_STRICT) != 0)
        {
            fprintf(stderr, "fourth mbind failed\n");
            return NULL;
        }
        if (cold_in_local < args->buf_size - args->hot_size &&
            mbind(a + cold_in_local, args->buf_size - args->hot_size - cold_in_local, MPOL_BIND, &remote_nodemask,
                  sizeof(remote_nodemask) * 8, MPOL_MF_STRICT) != 0)
        {
            fprintf(stderr, "fifth mbind failed\n");
            return NULL;
        }
    }

    // Fill buffer in reverse order, so that hot set pages fault and are allocated first (so that mbind policy can be
    // satisfied) Remaining memory will be used to opportunistically allocate cold set pages
    if (args->manual_placement)
    {
        // for(char *p = a + args->buf_size-1; p >= a; p--) {
        // *p = 'm';
        // asm volatile("" : : : "memory");
        // }
        memset(a, 'm', args->buf_size);
    }
    else
    {
        memset(a, 'm', args->buf_size);
    }

    asm volatile("" : : : "memory");

    if (args->manual_placement && args->reset_mbind)
    {
        // reset mbind policy to default
        if (mbind(a, args->buf_size, MPOL_DEFAULT, NULL, 0, 0) != 0)
        {
            fprintf(stderr, "reset mbind failed\n");
            return NULL;
        }
        fprintf(stderr, "resent mbind\n");
    }

    asm volatile("" : : : "memory");

    uint64_t x = 432437644 + args->thread_id;
    uint64_t count = 0, prev_count = 0;
    __m512i sum = _mm512_set_epi32(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    __m512i val = _mm512_set_epi32(1995, 1995, 2002, 2002, 1995, 1995, 2002, 2002, 1995, 1995, 2002, 2002, 1995, 1995,
                                   2002, 2002);

    int i;
    char *hot_start = a + (args->buf_size - args->hot_size);
    // char *cold_start = a;
    size_t hot_slots = args->hot_size / 64;
    size_t cold_slots = (args->buf_size) / 64;
    char *start;
    size_t slots;
    char *chunk;



    int* zipf_table_hot = (int*)numa_alloc_local(hot_slots * sizeof(int));
    if (zipf_table_hot == NULL)
    {
        fprintf(stderr, "malloc failed for zipf_table_hot\n");
        return NULL;
    }
    int *zipf_table_cold = (int*)numa_alloc_local(cold_slots * sizeof(int));
    if (zipf_table_cold == NULL)
    {
        fprintf(stderr, "malloc failed for zipf_table_cold\n");
        free(zipf_table_hot);
        return NULL;
    }
    
    precompute_zipf(hot_slots, ZIPF_ALPHA, zipf_table_hot, hot_slots);
    precompute_zipf(cold_slots, ZIPF_ALPHA, zipf_table_cold, cold_slots);

    // Flush the cache
    for (int i = 0; i < args->buf_size; i += 64)
    {
        _mm_clflush(a + i);
    }

    while (count < 999999999999999ULL)
    {
#if defined(CACHE_FRIENDLY_READWRITE)
#define BLOCK_SIZE 256 // Process data in cache-friendly blocks

        for (int block = 0; block < 131072 / BLOCK_SIZE; block++)
        {
            int block_offset = block * BLOCK_SIZE;

            for (int i = 0; i < BLOCK_SIZE; i++)
            {
                start = (xorshift64(&x) % 100 < HOTNESS) ? hot_start : a;
                slots = (xorshift64(&x) % 100 < HOTNESS) ? hot_slots : cold_slots;

                int zipf_index = (xorshift64(&x) % 100 < HOTNESS) ? zipfian_sample(zipf_table_hot, hot_slots, &x) :
                                                                   zipfian_sample(zipf_table_cold, cold_slots, &x);

                uint8_t *chunk = start + 64 * zipf_index;

                __m512i mm_a = _mm512_load_si512(chunk);
                _mm512_store_si512(chunk, _mm512_add_epi32(mm_a, _mm512_set1_epi32(1)));

                count++;
            }
        }
#elif defined(WORKLOAD_READWRITE)
        for (i = 0; i < 131072; i++)
        {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            start = (x % 100 < HOTNESS) ? (hot_start) : (a);
            slots = (x % 100 < HOTNESS) ? (hot_slots) : (cold_slots);
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            chunk = start + 64 * (x % slots);
            __m512i mm_a = _mm512_load_si512(chunk);
            _mm512_store_si512(chunk, _mm512_add_epi32(mm_a, val));
            count++;
        }
#elif defined(WORKLOAD_READ)
        for (i = 0; i < 131072; i++)
        {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            start = (x % 100 < HOTNESS) ? (hot_start) : (a);
            slots = (x % 100 < HOTNESS) ? (hot_slots) : (cold_slots);
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            chunk = start + 64 * (x % slots);
            __m512i mm_a = _mm512_load_si512(chunk);
            sum = _mm512_add_epi32(sum, mm_a);
            count++;
        }
#elif defined(WORKLOAD_2TO1)
        for (i = 0; i < 65536; i++)
        {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            start = (x % 100 < HOTNESS) ? (hot_start) : (a);
            slots = (x % 100 < HOTNESS) ? (hot_slots) : (cold_slots);
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            chunk = start + 64 * (x % slots);
            __m512i mm_a = _mm512_load_si512(chunk);
            sum = _mm512_add_epi32(sum, mm_a);
            count++;
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            start = (x % 100 < HOTNESS) ? (hot_start) : (a);
            slots = (x % 100 < HOTNESS) ? (hot_slots) : (cold_slots);
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            chunk = start + 64 * (x % slots);
            mm_a = _mm512_load_si512(chunk);
            _mm512_store_si512(chunk, _mm512_add_epi32(mm_a, val));
            count++;
        }
#elif defined(WORKLOAD_3TO1)
        for (i = 0; i < 45000; i++)
        {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            start = (x % 100 < HOTNESS) ? (hot_start) : (a);
            slots = (x % 100 < HOTNESS) ? (hot_slots) : (cold_slots);
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            chunk = start + 64 * (x % slots);
            __m512i mm_a = _mm512_load_si512(chunk);
            sum = _mm512_add_epi32(sum, mm_a);
            count++;
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            start = (x % 100 < HOTNESS) ? (hot_start) : (a);
            slots = (x % 100 < HOTNESS) ? (hot_slots) : (cold_slots);
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            chunk = start + 64 * (x % slots);
            mm_a = _mm512_load_si512(chunk);
            sum = _mm512_add_epi32(sum, mm_a);
            count++;
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            start = (x % 100 < HOTNESS) ? (hot_start) : (a);
            slots = (x % 100 < HOTNESS) ? (hot_slots) : (cold_slots);
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            chunk = start + 64 * (x % slots);
            mm_a = _mm512_load_si512(chunk);
            _mm512_store_si512(chunk, _mm512_add_epi32(mm_a, val));
            count++;
        }
#else // Error
        fprintf(stderr, "Error: No workload defined\n");
        munmap(a, args->buf_size);
        return NULL;
#endif

        atomic_store(args->count_ptr, count);
        if (atomic_load(&(g_move_hotset)))
        {
            hot_start = a;
        }
        if (atomic_load(&(args->finish)))
        {
            if (munmap(a, args->buf_size) != 0)
            {
                printf("munmap failed\n");
            }
            return NULL;
        }
        // cur_ts = rdtscp();
        // printf("cur_ts: %lu, prev_ts: %lu\n", cur_ts, prev_ts);
        // if(cur_ts - prev_ts >= LOG_INTERVAL_MS*TSC_ratio*100*1e3) {
        //     printf("%lu %lu\n", cur_ts-begin_ts, count - prev_count);
        //     prev_ts = cur_ts;
        //     prev_count = count;
        // }
        // if(__builtin_expect(count % 1000 == 0, 0)) {
        //     cur_ts = rdtscp();
        //     if(__builtin_expect(cur_ts - prev_ts >= LOG_INTERVAL_MS*TSC_ratio*100*1e3, 0)) {
        //         printf("%lu %lu\n", cur_ts-begin_ts, count - prev_count);
        //         prev_ts = cur_ts;
        //         prev_count = count;
        //     }
        // }
    }

    uint64_t read_checksum;
    int chx0, chx1, chx2, chx3;
    __m128i chx;
    chx = _mm512_extracti32x4_epi32(sum, 0);
    chx0 = _mm_extract_epi32(chx, 0);
    chx1 = _mm_extract_epi32(chx, 1);
    chx2 = _mm_extract_epi32(chx, 2);
    chx3 = _mm_extract_epi32(chx, 3);
    read_checksum += chx0 + chx1 + chx2 + chx3;
    chx = _mm512_extracti32x4_epi32(sum, 1);
    chx0 = _mm_extract_epi32(chx, 0);
    chx1 = _mm_extract_epi32(chx, 1);
    chx2 = _mm_extract_epi32(chx, 2);
    chx3 = _mm_extract_epi32(chx, 3);
    read_checksum += chx0 + chx1 + chx2 + chx3;
    chx = _mm512_extracti32x4_epi32(sum, 2);
    chx0 = _mm_extract_epi32(chx, 0);
    chx1 = _mm_extract_epi32(chx, 1);
    chx2 = _mm_extract_epi32(chx, 2);
    chx3 = _mm_extract_epi32(chx, 3);
    read_checksum += chx0 + chx1 + chx2 + chx3;
    chx = _mm512_extracti32x4_epi32(sum, 3);
    chx0 = _mm_extract_epi32(chx, 0);
    chx1 = _mm_extract_epi32(chx, 1);
    chx2 = _mm_extract_epi32(chx, 2);
    chx3 = _mm_extract_epi32(chx, 3);
    read_checksum += chx0 + chx1 + chx2 + chx3;
    printf("checksum reached: %lu\n", read_checksum);
    int xyz;
    uint64_t wrchk = 0;
    for (xyz = 0; xyz < args->buf_size; xyz++)
    {
        wrchk += (int)(a[xyz]);
    }
    printf("wrchk: %lu\n", wrchk);

    return NULL;
}

int main(int argc, char *argv[])
{
    pg_size = 4096ULL;
    // pg_size = 2ULL * 1024ULL * 1024ULL;
    if (getenv("GUPS_HUGEPAGES") != NULL)
    {
        pg_size = 2ULL * 1024ULL * 1024ULL;
    }

    setbuf(stdout, NULL);
    int cores[16] = {1, 3, 5, 7, 9, 11, 13, 15, 17, 19, 21, 23, 25, 27, 29, 31};
    int num_threads = 0;
    int manual_placement = 0;
    float hotset_local_frac = 0.0;
    int placement_mode = 0;
    int reset_mbind = 0;
    size_t local_free = 0;
    int duration = 100000; // Default duration
    enum placement_mode_t { PLACEMENT_DISTRIBUTE, PLACEMENT_LOCALIZE };

    // Parse arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [OPTIONS]\n", argv[0]);
            printf("Options:\n");
            printf("  -t, --threads NUM     Number of threads to use\n");
            printf("  -m, --manual          Enable manual memory placement\n"); 
            printf("  -f, --fraction NUM    Fraction of hot set to place locally (0.0-1.0)\n");
            printf("  -p, --placement MODE  Placement mode: 'distribute' or 'localize'\n");
            printf("  -r, --reset           Reset memory binding after initial placement\n");
            printf("  -d, --duration NUM    Duration in seconds (default: 100000)\n");
            printf("  -h, --help            Show this help message\n");
            return 0;
        } else if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--threads") == 0) {
            if (++i >= argc) {
                printf("Error: Missing threads value\n");
                return 1;
            }
            num_threads = atoi(argv[i]);
        } else if (strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "--manual") == 0) {
            manual_placement = 1;
        } else if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--fraction") == 0) {
            if (++i >= argc) {
                printf("Error: Missing fraction value\n");
                return 1;
            }
            hotset_local_frac = atof(argv[i]);
        } else if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--placement") == 0) {
            if (++i >= argc) {
                printf("Error: Missing placement value\n");
                return 1;
            }
            if (strcmp(argv[i], "distribute") == 0) {
                placement_mode = PLACEMENT_DISTRIBUTE;
            } else if (strcmp(argv[i], "localize") == 0) {
                placement_mode = PLACEMENT_LOCALIZE;
            }
        } else if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--reset") == 0) {
            reset_mbind = 1;
        } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--duration") == 0) {
            if (++i >= argc) {
                printf("Error: Missing duration value\n");
                return 1;
            }
            duration = atoi(argv[i]);
        }
    }


    if (num_threads <= 0 || num_threads > MAX_THREADS) {
        printf("Number of threads must be between 1 and %d\n", MAX_THREADS);
        return 1;
    }

    if (manual_placement) {
        if (getenv("GUPS_MOVE") != NULL) {
            numa_node_size(LOCAL_NUMA, &local_free);
            printf("Free size: %lu\n", local_free);
            local_free -= 10 * pg_size; // Leave buffer
        }
    }

    int move_hotset = (getenv("GUPS_MOVE") != NULL);
    int move_time = 0;
    if (move_hotset && getenv("GUPS_MOVE_TIME") != NULL) {
        move_time = atoi(getenv("GUPS_MOVE_TIME"));
    }


    atomic_init(&g_move_hotset, 0);

    _Atomic uint64_t thread_counts[MAX_THREADS];
    pthread_t threads[MAX_THREADS];
    ThreadArgs thread_args[MAX_THREADS];
    cpu_set_t cpuset;

    if (manual_placement && placement_mode == PLACEMENT_DISTRIBUTE)
    {
        for (int i = 0; i < num_threads; i++)
        {
            thread_args[i].local_hot_pages = (int)(hotset_local_frac * ((HOTSS / pg_size) / ((size_t)num_threads)));
        }
    }
    else if (manual_placement && placement_mode == PLACEMENT_LOCALIZE)
    {
        size_t total_local_pages = (int)(hotset_local_frac * (HOTSS / pg_size));
        size_t per_thread_hot_pages = ((HOTSS / pg_size) / ((size_t)num_threads));
        for (int i = 0; i < num_threads; i++)
        {
            if (total_local_pages > 0)
            {
                size_t num_pages =
                    (total_local_pages < per_thread_hot_pages) ? (total_local_pages) : (per_thread_hot_pages);
                thread_args[i].local_hot_pages = num_pages;
                total_local_pages -= num_pages;
            }
            else
            {
                thread_args[i].local_hot_pages = 0;
            }
        }
    }

    for (int i = 0; i < num_threads; ++i)
    {
        thread_args[i].thread_id = i;
        thread_args[i].buf_size = ((WSS / pg_size) / ((size_t)num_threads)) * pg_size;
        thread_args[i].hot_size = ((HOTSS / pg_size) / ((size_t)num_threads)) * pg_size;
        atomic_init(&thread_counts[i], 0);
        thread_args[i].count_ptr = &thread_counts[i];
        thread_args[i].manual_placement = manual_placement;
        thread_args[i].reset_mbind = reset_mbind;
        thread_args[i].local_free = ((local_free / pg_size) / ((size_t)num_threads)) * pg_size;
        atomic_init(&(thread_args[i].finish), 0);

        CPU_ZERO(&cpuset);
        CPU_SET(cores[i], &cpuset);

        if (pthread_create(&threads[i], NULL, thread_function, &thread_args[i]) != 0)
        {
            perror("pthread_create");
            return 1;
        }

        if (pthread_setaffinity_np(threads[i], sizeof(cpu_set_t), &cpuset) != 0)
        {
            perror("pthread_setaffinity_np");
            return 1;
        }
    }

    uint64_t prev_op_count = 0;
    int elapsed = 0;
    int max_duration = duration;
    if (getenv("GUPS_DURATION") != NULL)
    {
        max_duration = atoi(getenv("GUPS_DURATION"));
    }


    const int ROLLING_WINDOW_SIZE = 30;    
    uint64_t *rolling_window = (uint64_t *)calloc(ROLLING_WINDOW_SIZE, sizeof(uint64_t));
    int window_idx = 0;
    
    while (elapsed < max_duration)
    {
        sleep(1);
        uint64_t cur_op_count = 0;
        for (int i = 0; i < num_threads; i++)
        {
            cur_op_count += atomic_load(&thread_counts[i]);
        }
        
        // Remove oldest value from sum and add new value
        rolling_window[window_idx] = cur_op_count - prev_op_count;
        window_idx = (window_idx + 1) % ROLLING_WINDOW_SIZE;

        printf("%lu\n", cur_op_count - prev_op_count);
        prev_op_count = cur_op_count;
        elapsed++;
        if (elapsed == move_time)
        {
            printf("moved hotset\n");
            atomic_store(&g_move_hotset, 1);
        }
    }    

    for (int i = 0; i < num_threads; i++)
    {
        atomic_store(&(thread_args[i].finish), 1);
    }

    for (int i = 0; i < num_threads; ++i)
    {
        if (pthread_join(threads[i], NULL) != 0)
        {
            perror("pthread_join");
            return 1;
        }
    }

    // Throughput: Rolling Sum * 128/1e9
    uint64_t rolling_sum = 0;
    for (int i = 0; i < ROLLING_WINDOW_SIZE; i++)
    {
        rolling_sum += rolling_window[i];
    }

    float throughput = ((float)rolling_sum * 128.0f) / (1e9f * (float)ROLLING_WINDOW_SIZE);
    printf("Throughput: %f GOps\n", throughput);

    free(rolling_window);
    
    return 0;
}
