#include "backend_iface.h"
#include <linux/kernel.h>
#include <asm/msr.h>

// CHA counters are MSR-based.  
//   The starting MSR address is 0x0E00 + 0x10*CHA
//   	Offset 0 is Unit Control -- mostly un-needed
//   	Offsets 1-4 are the Counter PerfEvtSel registers
//   	Offset 5 is Filter0	-- selects state for LLC lookup event (and TID, if enabled by bit 19 of PerfEvtSel)
//   	Offset 6 is Filter1 -- lots of filter bits, including opcode -- default if unused should be 0x03b, or 0x------33 if using opcode matching
//   	Offset 7 is Unit Status
//   	Offsets 8,9,A,B are the Counter count registers
#define CHA_MSR_PMON_BASE 0x0E00L
#define CHA_MSR_PMON_CTL_BASE 0x0E01L
#define CHA_MSR_PMON_FILTER0_BASE 0x0E05L
#define CHA_MSR_PMON_FILTER1_BASE 0x0E06L
#define CHA_MSR_PMON_STATUS_BASE 0x0E07L
#define CHA_MSR_PMON_CTR_BASE 0x0E08L
#define CHA_OFFSET 0x10

#define FILTER1_LOCAL_VAL ((0x182 << 20) + 1)
#define FILTER1_REMOTE_VAL ((0x182 << 20) + 2)
#define CTL_OCCUPANCY 0x404336
#define CTL_INSERTS 0x404335

#define NUM_CHA_BOXES 10
#define NUM_CHA_COUNTERS 4

static int core_mon;

int backend_init(struct backend_config* config) {
    int cha, ret;
    u32 msr_num;
    u64 msr_val;

    if(!config) {
        printk(KERN_ERR "hsw-native: invalid backend_config passed\n");
        return -1;
    }

    core_mon = config->core;
    pr_err("core mon %d\n", core_mon);

    for(cha = 0; cha < NUM_CHA_BOXES; cha++) {
        msr_num = CHA_MSR_PMON_FILTER0_BASE + (CHA_OFFSET * cha); // Filter0
        msr_val = 0x00000000; // default; no filtering
        ret = wrmsr_on_cpu(core_mon, msr_num, msr_val & 0xFFFFFFFF, msr_val >> 32);
        if(ret != 0) {
            printk(KERN_ERR "hsw-native: wrmsr FILTER0 failed\n");
            return -1;
        }

        msr_num = CHA_MSR_PMON_FILTER1_BASE + (CHA_OFFSET * cha); // Filter1
        msr_val = (cha%2==0) ? FILTER1_LOCAL_VAL : FILTER1_REMOTE_VAL;
        ret = wrmsr_on_cpu(core_mon, msr_num, msr_val & 0xFFFFFFFF, msr_val >> 32);
        if (ret != 0) {
            printk(KERN_ERR "hsw-native: wrmsr FILTER1 failed\n");
            return -1;
        }

        msr_num = CHA_MSR_PMON_CTL_BASE + (CHA_OFFSET * cha) + 0; //counter 0
        msr_val = CTL_OCCUPANCY;
        ret = wrmsr_on_cpu(core_mon, msr_num, msr_val & 0xFFFFFFFF, msr_val >> 32);
        if (ret != 0) {
            printk(KERN_ERR "hsw-native: wrmsr COUNTER 0 failed\n");
            return -1;
        }

        msr_num = CHA_MSR_PMON_CTL_BASE + (CHA_OFFSET * cha) + 1; // counter 1
        msr_val = CTL_INSERTS;
        ret = wrmsr_on_cpu(core_mon, msr_num, msr_val & 0xFFFFFFFF, msr_val >> 32);
        if (ret != 0) {
            printk(KERN_ERR "hsw-native: wrmsr COUNTER 1 failed\n");
            return -1;
        }
    }

    return 0;
}

int backend_destroy() {
    return 0;
}

u64 backend_read_occupancy(int tier) {
    int cha, ctr;
    u32 msr_num, msr_high, msr_low;
    cha = tier; // Default tier on CHA slice 0, Alternate tier on CHA slice 1
    ctr = 0; // Occupancy
    msr_num = CHA_MSR_PMON_CTR_BASE + (CHA_OFFSET * cha) + ctr;    
    rdmsr_on_cpu(core_mon, msr_num, &msr_low, &msr_high);
    return ((((u64)msr_high) << 32) | ((u64)msr_low));
}

u64 backend_read_inserts(int tier) {
    int cha, ctr;
    u32 msr_num, msr_high, msr_low;
    cha = tier; // Default tier on CHA slice 0, Alternate tier on CHA slice 1
    ctr = 1; // Inserts
    msr_num = CHA_MSR_PMON_CTR_BASE + (CHA_OFFSET * cha) + ctr;    
    rdmsr_on_cpu(core_mon, msr_num, &msr_low, &msr_high);
    return ((((u64)msr_high) << 32) | ((u64)msr_low));
}
