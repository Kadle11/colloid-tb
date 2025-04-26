#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/debugfs.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <linux/mm.h>
//#include <linux/memory-tiers.h>
#include <linux/delay.h>
#include "backend/backend_iface.h"

#define SPINPOLL // TODO: configure this
#define SAMPLE_INTERVAL_MS 10 // Only used if SPINPOLL is not set
#ifdef SPINPOLL
#define EWMA_EXP 5
#else
#define EWMA_EXP 1
#endif
#define PRECISION 10 // bits

extern int colloid_local_lat_gt_remote;
extern int colloid_nid_of_interest;

// #define CORE_MON 63
// #define LOCAL_NUMA 1
#define WORKER_BUDGET 1000000
#define LOG_SIZE 10000
#define MIN_LOCAL_LAT 100
#define MIN_REMOTE_LAT 300

#define NUM_TIERS 2
#define NUM_COUNTERS 2

u64 smoothed_occ_local, smoothed_inserts_local;
u64 smoothed_occ_remote, smoothed_inserts_remote;
u64 smoothed_lat_local, smoothed_lat_remote;

void thread_fun_poll_cha(struct work_struct *);
struct workqueue_struct *poll_cha_queue;
#ifdef SPINPOLL
struct work_struct poll_cha;
#else
DECLARE_DELAYED_WORK(poll_cha, thread_fun_poll_cha);
#endif

u64 cur_ctr_tsc[NUM_TIERS][NUM_COUNTERS], prev_ctr_tsc[NUM_TIERS][NUM_COUNTERS];
u64 cur_ctr_val[NUM_TIERS][NUM_COUNTERS], prev_ctr_val[NUM_TIERS][NUM_COUNTERS];
int terminate_mon;

struct log_entry {
    u64 tsc;
    u64 occ_local;
    u64 inserts_local;
    u64 occ_remote;
    u64 inserts_remote;
};

struct log_entry log_buffer[LOG_SIZE];
int log_idx;

static inline __attribute__((always_inline)) unsigned long rdtscp(void)
{
   unsigned long a, d, c;

   __asm__ volatile("rdtscp" : "=a" (a), "=d" (d), "=c" (c));

   return (a | (d << 32));
}

static void poll_cha_init(void) {
    int ret;
    struct backend_config config;

    config.core = CORE_MON;

    ret = backend_init(&config);
    
    if(ret != 0) {
        printk(KERN_ERR "backend init failed\n");
        return;
    }
}

static inline void sample_cha_ctr(int cha, int ctr) {
    // cha: tier; ctr = 0 => occupancy, ctr = 1 => inserts
    u64 val;
    if(ctr == 0) {
        val = backend_read_occupancy(cha);
    } else {
        val = backend_read_inserts(cha);
    }
    prev_ctr_val[cha][ctr] = cur_ctr_val[cha][ctr];
    cur_ctr_val[cha][ctr] = val;
    prev_ctr_tsc[cha][ctr] = cur_ctr_tsc[cha][ctr];
    cur_ctr_tsc[cha][ctr] = rdtscp();
}

// static void dump_log(void) {
//     int i;
//     pr_info("Dumping colloid mon log");
//     for(i = 0; i < LOG_SIZE; i++) {
//         printk("%llu %llu %llu %llu %llu\n", log_buffer[i].tsc, log_buffer[i].occ_local, log_buffer[i].inserts_local, log_buffer[i].occ_remote, log_buffer[i].inserts_remote);
//     }
// }

void thread_fun_poll_cha(struct work_struct *work) {
    int cpu = CORE_MON;
    #ifdef SPINPOLL
    u32 budget = WORKER_BUDGET;
    #else
    u32 budget = 1;
    #endif
    u64 cum_occ, delta_tsc, cur_occ, cur_inserts;
    u64 cur_lat_local, cur_lat_remote;
    
    while (budget) {
        // read counters and update state
        sample_cha_ctr(0, 0); // default tier occupancy
        sample_cha_ctr(0, 1); // default tier inserts
        sample_cha_ctr(1, 0);
        sample_cha_ctr(1, 1);

        cum_occ = cur_ctr_val[0][0] - prev_ctr_val[0][0];
        delta_tsc = cur_ctr_tsc[0][0] - prev_ctr_tsc[0][0];
        cur_occ = (cum_occ << PRECISION);
        cur_inserts = ((cur_ctr_val[0][1] - prev_ctr_val[0][1])<<PRECISION);
        WRITE_ONCE(smoothed_occ_local, (cur_occ + ((1<<EWMA_EXP) - 1)*smoothed_occ_local)>>EWMA_EXP);
        WRITE_ONCE(smoothed_inserts_local, (cur_inserts + ((1<<EWMA_EXP) - 1)*smoothed_inserts_local)>>EWMA_EXP);
        cur_lat_local = (smoothed_inserts_local > 0)?(smoothed_occ_local/smoothed_inserts_local):(MIN_LOCAL_LAT);
        cur_lat_local = (cur_lat_local > MIN_LOCAL_LAT)?(cur_lat_local):(MIN_LOCAL_LAT);
        WRITE_ONCE(smoothed_lat_local, cur_lat_local);

        cum_occ = cur_ctr_val[1][0] - prev_ctr_val[1][0];
        delta_tsc = cur_ctr_tsc[1][0] - prev_ctr_tsc[1][0];
        cur_occ = (cum_occ << PRECISION);
        cur_inserts = ((cur_ctr_val[1][1] - prev_ctr_val[1][1])<<PRECISION);
        WRITE_ONCE(smoothed_occ_remote, (cur_occ + ((1<<EWMA_EXP) - 1)*smoothed_occ_remote)>>EWMA_EXP);
        WRITE_ONCE(smoothed_inserts_remote, (cur_inserts + ((1<<EWMA_EXP) - 1)*smoothed_inserts_remote)>>EWMA_EXP);
        cur_lat_remote = (smoothed_inserts_remote > 0)?(smoothed_occ_remote/smoothed_inserts_remote):(MIN_REMOTE_LAT);
        WRITE_ONCE(smoothed_lat_remote, (cur_lat_remote > MIN_REMOTE_LAT)?(cur_lat_remote):(MIN_REMOTE_LAT));

        WRITE_ONCE(colloid_local_lat_gt_remote, (smoothed_lat_local > smoothed_lat_remote));

        budget--;
    }
    if(!READ_ONCE(terminate_mon)){
        #ifdef SPINPOLL
        queue_work_on(cpu, poll_cha_queue, &poll_cha);
        #else
        queue_delayed_work_on(cpu, poll_cha_queue, &poll_cha, msecs_to_jiffies(SAMPLE_INTERVAL_MS));
        #endif
    }
    else{
        return;
    }
}

static void init_mon_state(void) {
    int cha, ctr;
    for(cha = 0; cha < NUM_TIERS; cha++) {
        for(ctr = 0; ctr < NUM_COUNTERS; ctr++) {
            cur_ctr_tsc[cha][ctr] = 0;
            cur_ctr_val[cha][ctr] = 0;
            sample_cha_ctr(cha, ctr);
        }
    }
    log_idx = 0;
}

static ssize_t colloid_latencies_show(struct kobject *kobj,
        struct kobj_attribute *attr, char *buf)
{
    return sprintf(buf, "local %llu remote %llu\n",
        smoothed_lat_local, smoothed_lat_remote);
}
static struct kobj_attribute colloid_latency_attr =
__ATTR(latency, 0444, colloid_latencies_show, NULL);

static struct attribute *colloid_attr[] = {
    &colloid_latency_attr.attr,
};

static const struct attribute_group colloid_attr_group = {
    .attrs = colloid_attr,
};

struct kobject *colloid_kobj = NULL;

static int colloidmon_init(void)
{
    int i;
    int err;

    poll_cha_queue = alloc_workqueue("poll_cha_queue",  WQ_HIGHPRI | WQ_CPU_INTENSIVE, 0);
    if (!poll_cha_queue) {
        printk(KERN_ERR "Failed to create CHA workqueue\n");
        return -ENOMEM;
    }

    #ifdef SPINPOLL
    INIT_WORK(&poll_cha, thread_fun_poll_cha);
    #else
    INIT_DELAYED_WORK(&poll_cha, thread_fun_poll_cha);
    #endif
    poll_cha_init();
    pr_info("Programmed counters");
    // Initialize state
    init_mon_state();
    WRITE_ONCE(terminate_mon, 0);
    #ifdef SPINPOLL
    queue_work_on(CORE_MON, poll_cha_queue, &poll_cha);
    #else
    queue_delayed_work_on(CORE_MON, poll_cha_queue, &poll_cha, msecs_to_jiffies(SAMPLE_INTERVAL_MS));
    #endif

    WRITE_ONCE(colloid_nid_of_interest, LOCAL_NUMA);

    colloid_kobj = kobject_create_and_add("colloid", kernel_kobj);
    if (unlikely(!colloid_kobj)) {
        printk(KERN_ERR "Failed to create colloid kobj\n");
        return -ENOMEM;
    }

    err = sysfs_create_group(colloid_kobj, &colloid_attr_group);
    if (err) {
        printk(KERN_ERR "Failed to register colloid group\n");
        kobject_put(colloid_kobj);
        return err;
    }

    for(i = 0; i < 5; i++) {
        msleep(1000);
        printk("%llu %llu\n", READ_ONCE(smoothed_occ_local), READ_ONCE(smoothed_occ_remote));
    }
    
    return 0;
}
 
static void colloidmon_exit(void)
{
    WRITE_ONCE(terminate_mon, 1);
    msleep(5000);
    flush_workqueue(poll_cha_queue);
    destroy_workqueue(poll_cha_queue);

    if (colloid_kobj) {
        sysfs_remove_group(colloid_kobj, &colloid_attr_group);
        kobject_put(colloid_kobj);
    }

    // dump_log();

    pr_info("colloidmon exit");
}
 
module_init(colloidmon_init);
module_exit(colloidmon_exit);
MODULE_AUTHOR("Midhul");
MODULE_LICENSE("GPL");
