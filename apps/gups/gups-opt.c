#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <math.h>
#include <float.h>
#include <limits.h>
#include <sys/time.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <immintrin.h>
#include <fcntl.h>
#include <sched.h>
#include <pthread.h>
#include <stdatomic.h>
#include <numaif.h>
#include <numa.h>

#define LOG_INTERVAL_MS 1000
#define MAX_THREADS 32

#define LOCAL_NUMA 0
#define REMOTE_NUMA 1

#define PG_SIZE 4096ULL

#define WSS 77309411328ULL
#define HOTSS 21474836480ULL
#define HOTSET_THRESHOLD 99 // Percentage of accesses to hotset

// int TSC_ratio;
// uint64_t begin_ts;

#if defined(__AVX512F__)
// --- AVX-512 backend ---
#define VEC_TYPE       __m512i
#define VEC_LOAD(p)    _mm512_load_si512((__m512i const*)(p))
#define VEC_STORE(p,v) _mm512_store_si512((__m512i*)(p), (v))
#define VEC_ADD(a,b)   _mm512_add_epi32((a),(b))
#define VEC_SET1(x)    _mm512_set1_epi32(x)
#define VEC_EXTRACT_128(v, idx) _mm512_extracti32x4_epi32((v), (idx))
#else
// --- AVX2 fallback ---
#define VEC_TYPE       __m256i
#define VEC_LOAD(p)    _mm256_load_si256((__m256i const*)(p))
#define VEC_STORE(p,v) _mm256_store_si256((__m256i*)(p), (v))
#define VEC_ADD(a,b)   _mm256_add_epi32((a),(b))
#define VEC_SET1(x)    _mm256_set1_epi32(x)
#define VEC_EXTRACT_128(v, idx) \
    ((idx) == 0 ? _mm256_castsi256_si128(v) : _mm256_extracti128_si256(v, 1))

#endif

size_t pg_size;

static inline __attribute__((always_inline)) unsigned long rdtsc()
{
   unsigned long a, d;

   __asm__ volatile("rdtsc" : "=a" (a), "=d" (d));

   return (a | (d << 32));
}


static inline __attribute__((always_inline)) unsigned long rdtscp()
{
   unsigned long a, d, c;

   __asm__ volatile("rdtscp" : "=a" (a), "=d" (d), "=c" (c));

   return (a | (d << 32));
}

typedef struct {
    int thread_id;
    size_t buf_size;
    size_t hot_size;
    _Atomic uint64_t *count_ptr;
    int manual_placement;
    size_t local_hot_pages;
    int reset_mbind;
    size_t local_free;
    _Atomic int finish;
    size_t num_threads;
} ThreadArgs;

#define MAP_HUGE_1GB (30 << MAP_HUGE_SHIFT)

_Atomic int g_move_hotset;
_Atomic int g_sync;

void print_pages_nodes(void *addr, size_t length, char *label, int thread_id) {
    size_t pages = length / PG_SIZE;

    void **pages_addr = malloc(pages * sizeof(void *));
    int *status = malloc(pages * sizeof(int));
    size_t isOnNUMA0 = 0;
    size_t isOnNUMA1 = 0;

    for (size_t i = 0; i < pages; i++)
        pages_addr[i] = (char *)addr + i * PG_SIZE;

    if (move_pages(0, pages, pages_addr, NULL, status, 0) == -1) {
        perror("move_pages");
        exit(1);
    }

    for (size_t i = 0; i < pages; i++) {
        if (status[i] == LOCAL_NUMA) isOnNUMA0++;
        else if (status[i] == REMOTE_NUMA) isOnNUMA1++;
    }

    printf("%s/%d: Total pages: %zu, NUMA0: %zu, NUMA1: %zu\n", label, thread_id, pages, isOnNUMA0, isOnNUMA1);

    free(pages_addr);
    free(status);
}

void *thread_function(void *arg) {
    ThreadArgs *args = (ThreadArgs *)arg;
    // char *a = (char *)malloc(args->buf_size);
    int mmap_flags =  MAP_PRIVATE | MAP_ANONYMOUS;
    if(getenv("GUPS_HUGEPAGES") != NULL) {
        mmap_flags |= MAP_HUGETLB;
    }
    if(getenv("GUPS_HUGEPAGES_1GB") != NULL) {
        mmap_flags |= MAP_HUGETLB;
        mmap_flags |= MAP_HUGE_1GB;
    }
    char *a = mmap(0, args->hot_size, PROT_READ | PROT_WRITE, mmap_flags, -1, 0);
    char *b = mmap(0, args->buf_size - args->hot_size, PROT_READ | PROT_WRITE, mmap_flags, -1, 0); 
    if(a == NULL) {
        printf("mmap failed\n");
        return NULL;
    }

    uint64_t cur_ts=0, prev_ts=0;
    cur_ts = rdtscp();
    prev_ts = cur_ts;


    char *hot_start = a;
    char *cold_start = b;
    size_t hot_slots = args->hot_size / 64;
    size_t cold_slots = (args->buf_size - args->hot_size) / 64;

    if(args->manual_placement) {

        size_t hotset_in_local = args->local_hot_pages * PG_SIZE;
        size_t hotset_in_remote = args->hot_size - hotset_in_local;
        size_t coldset_in_local = args->local_free - hotset_in_local;
        size_t coldset_in_remote = (args->buf_size - args->hot_size) - coldset_in_local;

        // Touch data in local NUMA node
        for(size_t offset = 0; offset < hotset_in_local; offset += PG_SIZE) {
            memset(&hot_start[offset], 1995, 64);
            volatile char tmp = hot_start[offset];
        }

        for(size_t offset = 0; offset < coldset_in_local; offset += PG_SIZE) {
            memset(&cold_start[offset], 1996, 64);
            volatile char tmp = cold_start[offset];
        }

        // Barrier to ensure all threads finish touching local data
        atomic_fetch_add(&g_sync, 1);
        while(atomic_load(&g_sync) < args->num_threads) {
            // busy wait
            sched_yield();
        }

        // Touch data in remote NUMA node
        for(size_t offset = 0; offset < coldset_in_remote; offset += PG_SIZE) {
            memset(&cold_start[coldset_in_local + offset], 1996, 64);
            volatile char tmp = cold_start[coldset_in_local + offset];
        }


        for(size_t offset = 0; offset < hotset_in_remote; offset += PG_SIZE) {
            memset(&hot_start[hotset_in_local + offset], 1995, 64);
            volatile char tmp = hot_start[hotset_in_local + offset];
        }

        // Verify placement
        print_pages_nodes(hot_start, hotset_in_local, "Hotset Local", args->thread_id);
        print_pages_nodes(cold_start, coldset_in_local, "Coldset Local", args->thread_id);
        print_pages_nodes(hot_start + hotset_in_local, hotset_in_remote, "Hotset Remote", args->thread_id);
        print_pages_nodes(cold_start + coldset_in_local, coldset_in_remote, "Coldset Remote", args->thread_id);
    }
        
    uint64_t x = 432437644 + args->thread_id;
    uint64_t count = 0, prev_count = 0;
    VEC_TYPE sum = VEC_SET1(0);
    VEC_TYPE val = VEC_SET1(1995);
    int i;
    char *start;
    size_t slots;
    char *chunk;

    while(count < 999999999999999ULL) {
        #if defined(WORKLOAD_READWRITE)
        for(i = 0; i < 131072; i++) {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            start = (x%100 < HOTSET_THRESHOLD)?(hot_start):(cold_start);
            slots = (x%100 < HOTSET_THRESHOLD)?(hot_slots):(cold_slots);
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            chunk = start + 64*(x%slots);
            VEC_TYPE mm_a = VEC_LOAD(chunk);
            VEC_STORE(chunk, VEC_ADD(mm_a, val));
            count++;
        }
        #elif defined(WORKLOAD_READ)
        for(i = 0; i < 131072; i++) {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            start = (x%100 < HOTSET_THRESHOLD)?(hot_start):(cold_start);
            slots = (x%100 < HOTSET_THRESHOLD)?(hot_slots):(cold_slots);
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            chunk = start + 64*(x%slots);
            VEC_TYPE mm_a = VEC_LOAD(chunk);
            sum = VEC_ADD(sum, mm_a);
            count++;
        }
        #elif defined(WORKLOAD_2TO1)
        for(i = 0; i < 65536; i++) {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            start = (x%100 < HOTSET_THRESHOLD)?(hot_start):(cold_start);
            slots = (x%100 < HOTSET_THRESHOLD)?(hot_slots):(cold_slots);
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            chunk = start + 64*(x%slots);
            VEC_TYPE mm_a = VEC_LOAD(chunk);
            sum = VEC_ADD(sum, mm_a);
            count++;
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            start = (x%100 < HOTSET_THRESHOLD)?(hot_start):(cold_start);
            slots = (x%100 < HOTSET_THRESHOLD)?(hot_slots):(cold_slots);
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            chunk = start + 64*(x%slots);
            mm_a = VEC_LOAD(chunk);
            VEC_STORE(chunk, VEC_ADD(mm_a, val));
            count++;
        }
        #elif defined(WORKLOAD_3TO1)
        for(i = 0; i < 45000; i++) {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            start = (x%100 < HOTSET_THRESHOLD)?(hot_start):(cold_start);
            slots = (x%100 < HOTSET_THRESHOLD)?(hot_slots):(cold_slots);
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            chunk = start + 64*(x%slots);
            VEC_TYPE mm_a = VEC_LOAD(chunk);
            sum = VEC_ADD(sum, mm_a);
            count++;
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            start = (x%100 < HOTSET_THRESHOLD)?(hot_start):(cold_start);
            slots = (x%100 < HOTSET_THRESHOLD)?(hot_slots):(cold_slots);
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            chunk = start + 64*(x%slots);
            mm_a = VEC_LOAD(chunk);
            sum = VEC_ADD(sum, mm_a);
            count++;
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            start = (x%100 < HOTSET_THRESHOLD)?(hot_start):(cold_start);
            slots = (x%100 < HOTSET_THRESHOLD)?(hot_slots):(cold_slots);
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            chunk = start + 64*(x%slots);
            mm_a = VEC_LOAD(chunk);
            VEC_STORE(chunk, VEC_ADD(mm_a, val));
            count++;
        }
        #endif

        
        atomic_store(args->count_ptr, count);
        if(atomic_load(&(g_move_hotset))) {
            hot_start = a;
        }

        if(atomic_load(&(args->finish))) {

            // Check if Placement has changed
            if(args->manual_placement) {
                size_t hotset_in_local = args->local_hot_pages * PG_SIZE;
                size_t hotset_in_remote = args->hot_size - hotset_in_local;
                size_t coldset_in_local = args->local_free - hotset_in_local;
                size_t coldset_in_remote = (args->buf_size - args->hot_size) - coldset_in_local;

                print_pages_nodes(hot_start, hotset_in_local, "Hotset Local", args->thread_id);
                print_pages_nodes(cold_start, coldset_in_local, "Coldset Local", args->thread_id);
                print_pages_nodes(hot_start + hotset_in_local, hotset_in_remote, "Hotset Remote", args->thread_id);
                print_pages_nodes(cold_start + coldset_in_local, coldset_in_remote, "Coldset Remote", args->thread_id);
            }
        
            if(munmap(a, args->hot_size) != 0) {
                printf("munmap failed\n");
            }

            if(munmap(b, args->buf_size - args->hot_size) != 0) {
                printf("munmap failed\n");
            }

		    return NULL;
        }

    }


    uint64_t read_checksum;
    int chx0, chx1, chx2, chx3;
    __m128i chx;
    chx = VEC_EXTRACT_128(sum, 0);
    chx0 = _mm_extract_epi32(chx, 0);
    chx1 = _mm_extract_epi32(chx, 1);
    chx2 = _mm_extract_epi32(chx, 2);
    chx3 = _mm_extract_epi32(chx, 3);
    read_checksum += chx0 + chx1 + chx2 + chx3;
    chx = VEC_EXTRACT_128(sum, 1);
    chx0 = _mm_extract_epi32(chx, 0);
    chx1 = _mm_extract_epi32(chx, 1);
    chx2 = _mm_extract_epi32(chx, 2);
    chx3 = _mm_extract_epi32(chx, 3);
    read_checksum += chx0 + chx1 + chx2 + chx3;
    chx = VEC_EXTRACT_128(sum, 2);
    chx0 = _mm_extract_epi32(chx, 0);
    chx1 = _mm_extract_epi32(chx, 1);
    chx2 = _mm_extract_epi32(chx, 2);
    chx3 = _mm_extract_epi32(chx, 3);
    read_checksum += chx0 + chx1 + chx2 + chx3;
    chx = VEC_EXTRACT_128(sum, 3);
    chx0 = _mm_extract_epi32(chx, 0);
    chx1 = _mm_extract_epi32(chx, 1);
    chx2 = _mm_extract_epi32(chx, 2);
    chx3 = _mm_extract_epi32(chx, 3);
    read_checksum += chx0 + chx1 + chx2 + chx3;
    printf("checksum reached: %lu\n", read_checksum);

    int xyz;
    uint64_t wrchk = 0;
    for(xyz = 0; xyz < args->buf_size; xyz++) {
        wrchk += (int)(a[xyz]);
    }
    printf("wrchk: %lu\n", wrchk);
    
    return NULL;
}

int main(int argc, char *argv[]) {
    pg_size = 4096ULL;
    if(getenv("GUPS_HUGEPAGES") != NULL) {
        pg_size = 2ULL*1024ULL*1024ULL;
    }
    setbuf(stdout, NULL);
    int cores[32] = {0,2,4,6,8,10,12,14,16,18,20,22,24,26};
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <num_threads> [manual] [fraction of hotset in local] [distribute/localize] [reset]\n", argv[0]);
        return 1;
    }

    int num_threads = atoi(argv[1]);
    if (num_threads <= 0 || num_threads > MAX_THREADS) {
        fprintf(stderr, "Number of threads invalid\n");
        return 1;
    }

    int manual_placement = 0;
    float hotset_local_frac = 0.0;
    int placement_mode = 0;
    int reset_mbind = 0;
    size_t local_free = 0;
    enum {
        PLACEMENT_DISTRIBUTE,
        PLACEMENT_LOCALIZE
    };
    if(argc >= 3 && strncmp(argv[2], "manual", sizeof("manual")) == 0) {
        if(argc < 5) {
            fprintf(stderr, "Usage: %s <num_threads> [manual] [fraction of hotset in local] [distribute/localize] [reset]\n", argv[0]);
            return 1;
        }
        manual_placement = 1;
        hotset_local_frac = atof(argv[3]);
        if(strncmp(argv[4], "distribute", sizeof("distribute")) == 0) {
            placement_mode = PLACEMENT_DISTRIBUTE;
        } else if(strncmp(argv[4], "localize", sizeof("localize")) == 0) {
            placement_mode = PLACEMENT_LOCALIZE;
        } else {
            fprintf(stderr, "Unknown manual placement mode\n");
            return 1;
        }
        if(argc >= 6 && strncmp(argv[5], "reset", sizeof("reset")) == 0) {
            reset_mbind = 1;
        }

        numa_node_size(LOCAL_NUMA, &local_free);
        printf("Free size: %lu\n", local_free);
        local_free -= 10*pg_size; // Leave buffer
    }

    int move_hotset = 0;
    int move_time = 0;

    if(argc >= 3 && strncmp(argv[2], "move", sizeof("move")) == 0) {
        if(argc < 4) {
            fprintf(stderr, "Usage: %s <num_threads> [move] [move time]\n", argv[0]);
            return 1;
        }
        move_hotset = 1;
        move_time = atoi(argv[3]);
    }
    atomic_init(&g_move_hotset, 0);
    atomic_init(&g_sync, 0);

    // Get TSC frequency
    // int msr_fd;
    // ssize_t ret;
    // uint64_t msr_val;
    // msr_fd = open("/dev/cpu/0/msr", O_RDWR);
    // if(msr_fd == -1) {
    //     fprintf(stderr, "An error occurred while opening msr file.\n");
	// 	return EXIT_FAILURE;
    // }
    // ret = pread(msr_fd, &msr_val, sizeof(msr_val), 0xCEL);
    // TSC_ratio = (msr_val & 0x000000000000ff00L) >> 8;

    _Atomic uint64_t thread_counts[MAX_THREADS];
    pthread_t threads[MAX_THREADS];
    ThreadArgs thread_args[MAX_THREADS];
    cpu_set_t cpuset;

    if(manual_placement && placement_mode == PLACEMENT_DISTRIBUTE) {
        for(int i = 0; i < num_threads; i++) {
            thread_args[i].local_hot_pages = (int)(hotset_local_frac*((HOTSS/pg_size)/((size_t)num_threads)));
        }
    } else if(manual_placement && placement_mode == PLACEMENT_LOCALIZE) {
        size_t total_local_pages = (int)(hotset_local_frac*(HOTSS/pg_size));
        size_t per_thread_hot_pages = ((HOTSS/pg_size)/((size_t)num_threads));
        for(int i = 0; i < num_threads; i++) {
            if(total_local_pages > 0){
                size_t num_pages = (total_local_pages < per_thread_hot_pages)?(total_local_pages):(per_thread_hot_pages);
                thread_args[i].local_hot_pages = num_pages;
                total_local_pages -= num_pages; 
            } else {
                thread_args[i].local_hot_pages = 0;
            }
        }
    }

    for (int i = 0; i < num_threads; ++i) {
        thread_args[i].thread_id = i;
        thread_args[i].buf_size = ((WSS/pg_size)/((size_t)num_threads))*pg_size;
        thread_args[i].hot_size = ((HOTSS/pg_size)/((size_t)num_threads))*pg_size;
        atomic_init(&thread_counts[i], 0);
        thread_args[i].count_ptr = &thread_counts[i];
        thread_args[i].manual_placement = manual_placement;
        thread_args[i].reset_mbind = reset_mbind;
        thread_args[i].local_free = ((local_free/pg_size)/((size_t)num_threads))*pg_size;
        atomic_init(&(thread_args[i].finish), 0);
        thread_args[i].num_threads = num_threads;
        
        CPU_ZERO(&cpuset);
        CPU_SET(cores[i], &cpuset);

        if (pthread_create(&threads[i], NULL, thread_function, &thread_args[i]) != 0) {
            perror("pthread_create");
            return 1;
        }

        if (pthread_setaffinity_np(threads[i], sizeof(cpu_set_t), &cpuset) != 0) {
            perror("pthread_setaffinity_np");
            return 1;
        }
    }

    uint64_t prev_op_count = 0;
    int elapsed = 0;
    int max_duration = 100000;
    if(getenv("GUPS_DURATION") != NULL) {
        max_duration = atoi(getenv("GUPS_DURATION"));
    }

    while(elapsed < max_duration) {
        sleep(1);
        uint64_t cur_op_count = 0;
        for(int i = 0; i < num_threads; i++) {
            cur_op_count += atomic_load(&thread_counts[i]);
        }
        printf("%lu\n", cur_op_count - prev_op_count);
        prev_op_count = cur_op_count;
        elapsed++;
        if(elapsed == move_time) {
            printf("moved hotset\n");
            atomic_store(&g_move_hotset, 1);
        }
    }

    for(int i = 0; i < num_threads; i++) {
        atomic_store(&(thread_args[i].finish), 1);
    }

    for (int i = 0; i < num_threads; ++i) {
        if (pthread_join(threads[i], NULL) != 0) {
            perror("pthread_join");
            return 1;
        }
    }

    return 0;
}
