#!/bin/bash

# Make sure tiering is initialized
# Make sure to check if THP is enabled or not

# set -x
# set -e

ARCH=icx
COLLOID_HOME=/proj/prismgt-PG0/vrao79/colloid-tb
mio_path=/proj/prismgt-PG0/vrao79/understanding-the-host-network

record_path=$COLLOID_HOME/colloid-stats
stats_path=$COLLOID_HOME/apps/memory_microbenchmarks/stats
memeater_path=$COLLOID_HOME/tpp/memeater
kswapdrst_path=$COLLOID_HOME/tpp/kswapdrst
scripts_path=$COLLOID_HOME/scripts

bandwidth_bm_path=$COLLOID_HOME/apps/memory_microbenchmarks/build/bin/bandwidth_benchmark
antagonist_path=$COLLOID_HOME/apps/memory_microbenchmarks/scripts/inject_cross_numa.sh

local_numa=1
local_size=32768

config=$1
duration=$2
app_cores=$3

icx_all_core_list="1,3,5,7,9,11,13,15,17,19,21,23,25,27,29,31,33,35,37,39,41,43,45,47,49,51,53,55,57,59,61,63,65,67,69,71"
hsw_all_core_list="1,3,5,7,9,11,13,15,17,19,21,23,25,27"

MIO_STATS="--stats_colloid_mbm --stats_colloid_wait 570"

# Define BG_CORES based on Input Platform (ICX or HSW)
all_core_list=""
if [[ $ARCH == *"icx"* ]]; then
    all_core_list=$icx_all_core_list
else
    all_core_list=$hsw_all_core_list
fi

bg_core_list=$(echo "$all_core_list" | cut -d ',' -f $((app_cores + 1))-)
echo "Background core list: $bg_core_list"

app_core_list=$(echo "$all_core_list" | cut -d ',' -f 1-$app_cores)
echo "App core list: $app_core_list"


is_process_alive() {
    pid=$1
    if ps -p "$pid" > /dev/null 2>&1; then
        state=$(ps -o stat= -p "$pid" | awk '{print $1}')
        if [[ "$state" =~ Z ]]; then
            echo "Process $pid is a zombie."
            return 1
        else
            # echo "Process $pid is alive."
            return 0
        fi
    else
        echo "Process $pid does not exist."
        return 1
    fi
}

# for bg_cores in 0 6; do

#     all_pids=()
#     config=$1-app$app_cores-bg$bg_cores

#     function cleanup() {
#         for pid in "${all_pids[@]}"; do
#             kill -9 $pid > /dev/null 2>&1;
#         done;
#         killall perf > /dev/null 2>&1;
#         killall python3 > /dev/null 2>&1;
#         killall stream > /dev/null 2>&1;
#         killall bpftrace > /dev/null 2>&1;
#         rmmod memeater.ko > /dev/null 2>&1;
#         rmmod kswapdrst.ko > /dev/null 2>&1;
#         rmmod colloid-mon.ko > /dev/null 2>&1;
#         $scripts_path/disable_thp.sh
#         echo "Cleaned up";
#     }

#     trap cleanup EXIT
#     cleanup

#     # Shrink Local Memory 
#     sudo rmmod memeater.ko > /dev/null 2>&1;
#     if [ -n "${ENABLE_THP}" ]; then
#         echo "Loading memeater + THP"; 
#         sudo insmod $memeater_path/memeater.ko sizeMiB=$(numastat -m | grep MemFree | awk -v nidx=$local_numa -v sz=$local_size '{print int($(2+nidx)-sz)}') PGSIZE=2097152 PGORDER=9;

#     else
#         echo "Loading memeater";
#         sudo insmod $memeater_path/memeater.ko sizeMiB=$(numastat -m | grep MemFree | awk -v nidx=$local_numa -v sz=$local_size '{print int($(2+nidx)-sz)}');
#     fi 

#     sudo sync;
#     echo 3 | sudo tee /proc/sys/vm/drop_caches

#     if [ -n "${ENABLE_THP}" ]; then
#         echo "Enabling THP";
#         $scripts_path/enable_thp.sh;
#     fi

#     # Run kswapdrst
#     echo "Running kswapdrst"
#     sudo insmod $kswapdrst_path/kswapdrst.ko

#     # Make sure swap is disabled
#     swapoff -a
#     echo 1 | sudo tee /sys/kernel/mm/numa/demotion_enabled
#     echo 2 | sudo tee /proc/sys/kernel/numa_balancing

#     mio_opts=( $MIO_STATS )
#     if [ $bg_cores -gt 0 ]; then
#         echo "Running bg traffic on $bg_cores"
#         PYTHONPATH=$PYTHONPATH:$mio_path python3 -m mio $config-mio --ant_cpus $bg_core_list --ant_num_cores $bg_cores --ant_mem_numa 1 --ant stream --ant_writefrac 50 --ant_inst_size 64 --ant_duration 10000 "${mio_opts[@]}" &
#         pid_mio=$!;
#         all_pids+=($pid_mio);
#         sleep 7;
#     fi

#     echo "Local mem size"
#     echo $(numastat -m | grep MemFree)
#     echo $(numastat -m | grep MemFree) > $stats_path/$config.memfree.txt

#     # BPFTRACE
#     sudo bpftrace -e 'tracepoint:raw_syscalls:sys_enter { @[comm] = count(); }' > $record_path/bpftrace.txt &
#     pid_bpftrace=$!;
#     all_pids+=($pid_bpftrace);

#     cat /proc/vmstat > $stats_path/$config.before_vmstat.txt

#     # SAR
#     sar_logfile=$stats_path/$config.sar.txt
#     sar -u -P ALL 1 > $sar_logfile 2>&1 &
#     pid_sar=$!;
#     all_pids+=($pid_sar);

#     # Run Latency Benchmark
#     echo "Running $config"
#     numactl --membind=$local_numa -C $app_core_list $bandwidth_bm_path --threads 15 --duration 120 --warmup 30 --dist zipf > $stats_path/$config.app.txt 2> $stats_path/$config.stderr.txt &
#     pid_app=$!;
#     all_pids+=($pid_app);

#     # Record vm stats for duration
#     rm -f $stats_path/$config.vmstat.txt

#     if [ $duration -gt 0 ]; then
#         for i in $(seq 1 1 $duration); do
#             grep -E "pgdemote|pgpromote|pgmigrate|thp_migration" /proc/vmstat >> $stats_path/$config.vmstat.txt
#             sleep 1;
#         done;
#     else
#         while is_process_alive $pid_app; do
#             grep -E "pgdemote|pgpromote|pgmigrate|thp_migration" /proc/vmstat >> $stats_path/$config.vmstat.txt
#             sleep 1;
#         done;
#     fi

#     if [ $duration -gt 0 ]; then
#         kill $pid_app > /dev/null 2>&1;
#         while is_process_alive $pid_app; do
#             sleep 1;
#         done;
#     fi

#     # Stop sar monitoring
#     kill $pid_sar > /dev/null 2>&1;
#     while is_process_alive $pid_sar; do
#         sleep 1;
#     done;
#     killall sar > /dev/null 2>&1;

#     cat /proc/vmstat > $stats_path/$config.after_vmstat.txt

#     # Stop bpftrace monitoring
#     killall bpftrace > /dev/null 2>&1;
#     while is_process_alive $pid_bpftrace; do
#         sleep 1;
#     done;

#     if [ $bg_cores -gt 0 ] || [ "${#mio_opts[@]}" -gt 0 ]; then
#         kill $pid_mio > /dev/null 2>&1;
#         while is_process_alive $pid_mio; do
#                 sleep 1;
#         done;
#         killall python3 > /dev/null 2>&1;
#         killall stream > /dev/null 2>&1;
#     fi

#     rmmod kswapdrst.ko > /dev/null 2>&1;
#     rmmod memeater.ko > /dev/null 2>&1;

#     $scripts_path/disable_thp.sh;
#     echo "Done";

# done

# # Non-Temporal
# for bg_cores in 0 6; do

#     all_pids=()
#     config=$1-app$app_cores-bg$bg_cores-nt

#     function cleanup() {
#         for pid in "${all_pids[@]}"; do
#             kill -9 $pid > /dev/null 2>&1;
#         done;
#         killall perf > /dev/null 2>&1;
#         killall python3 > /dev/null 2>&1;
#         killall stream > /dev/null 2>&1;
#         killall bpftrace > /dev/null 2>&1;
#         rmmod memeater.ko > /dev/null 2>&1;
#         rmmod kswapdrst.ko > /dev/null 2>&1;
#         rmmod colloid-mon.ko > /dev/null 2>&1;
#         $scripts_path/disable_thp.sh
#         echo "Cleaned up";
#     }

#     trap cleanup EXIT
#     cleanup

#     # Shrink Local Memory 
#     sudo rmmod memeater.ko > /dev/null 2>&1;
#     if [ -n "${ENABLE_THP}" ]; then
#         echo "Loading memeater + THP"; 
#         sudo insmod $memeater_path/memeater.ko sizeMiB=$(numastat -m | grep MemFree | awk -v nidx=$local_numa -v sz=$local_size '{print int($(2+nidx)-sz)}') PGSIZE=2097152 PGORDER=9;

#     else
#         echo "Loading memeater";
#         sudo insmod $memeater_path/memeater.ko sizeMiB=$(numastat -m | grep MemFree | awk -v nidx=$local_numa -v sz=$local_size '{print int($(2+nidx)-sz)}');
#     fi 

#     sudo sync;
#     echo 3 | sudo tee /proc/sys/vm/drop_caches

#     if [ -n "${ENABLE_THP}" ]; then
#         echo "Enabling THP";
#         $scripts_path/enable_thp.sh;
#     fi

#     # Run kswapdrst
#     echo "Running kswapdrst"
#     sudo insmod $kswapdrst_path/kswapdrst.ko

#     # Make sure swap is disabled
#     swapoff -a
#     echo 1 | sudo tee /sys/kernel/mm/numa/demotion_enabled
#     echo 2 | sudo tee /proc/sys/kernel/numa_balancing

#     mio_opts=( $MIO_STATS )
#     if [ $bg_cores -gt 0 ]; then
#         echo "Running bg traffic on $bg_cores"
#         PYTHONPATH=$PYTHONPATH:$mio_path python3 -m mio $config-mio --ant_cpus $bg_core_list --ant_num_cores $bg_cores --ant_mem_numa 1 --ant stream --ant_writefrac 50 --ant_inst_size 64 --ant_duration 10000 "${mio_opts[@]}" &
#         pid_mio=$!;
#         all_pids+=($pid_mio);
#         sleep 7;
#     fi

#     echo "Local mem size"
#     echo $(numastat -m | grep MemFree)
#     echo $(numastat -m | grep MemFree) > $stats_path/$config.memfree.txt

#     # BPFTRACE
#     sudo bpftrace -e 'tracepoint:raw_syscalls:sys_enter { @[comm] = count(); }' > $record_path/bpftrace.txt &
#     pid_bpftrace=$!;
#     all_pids+=($pid_bpftrace);

#     cat /proc/vmstat > $stats_path/$config.before_vmstat.txt

#     # SAR
#     sar_logfile=$stats_path/$config.sar.txt
#     sar -u -P ALL 1 > $sar_logfile 2>&1 &
#     pid_sar=$!;
#     all_pids+=($pid_sar);

#     # Run Latency Benchmark
#     echo "Running $config"
#     numactl --membind=$local_numa -C $app_core_list $bandwidth_bm_path --threads 15 --duration 120 --warmup 30 --dist zipf -n 1 > $stats_path/$config.app.txt 2> $stats_path/$config.stderr.txt &
#     pid_app=$!;
#     all_pids+=($pid_app);

#     # Record vm stats for duration
#     rm -f $stats_path/$config.vmstat.txt

#     if [ $duration -gt 0 ]; then
#         for i in $(seq 1 1 $duration); do
#             grep -E "pgdemote|pgpromote|pgmigrate|thp_migration" /proc/vmstat >> $stats_path/$config.vmstat.txt
#             sleep 1;
#         done;
#     else
#         while is_process_alive $pid_app; do
#             grep -E "pgdemote|pgpromote|pgmigrate|thp_migration" /proc/vmstat >> $stats_path/$config.vmstat.txt
#             sleep 1;
#         done;
#     fi

#     if [ $duration -gt 0 ]; then
#         kill $pid_app > /dev/null 2>&1;
#         while is_process_alive $pid_app; do
#             sleep 1;
#         done;
#     fi

#     # Stop sar monitoring
#     kill $pid_sar > /dev/null 2>&1;
#     while is_process_alive $pid_sar; do
#         sleep 1;
#     done;
#     killall sar > /dev/null 2>&1;

#     cat /proc/vmstat > $stats_path/$config.after_vmstat.txt

#     # Stop bpftrace monitoring
#     killall bpftrace > /dev/null 2>&1;
#     while is_process_alive $pid_bpftrace; do
#         sleep 1;
#     done;

#     if [ $bg_cores -gt 0 ] || [ "${#mio_opts[@]}" -gt 0 ]; then
#         kill $pid_mio > /dev/null 2>&1;
#         while is_process_alive $pid_mio; do
#                 sleep 1;
#         done;
#         killall python3 > /dev/null 2>&1;
#         killall stream > /dev/null 2>&1;
#     fi

#     rmmod kswapdrst.ko > /dev/null 2>&1;
#     rmmod memeater.ko > /dev/null 2>&1;

#     $scripts_path/disable_thp.sh;
#     echo "Done";

# done;

# Antagonist

${antagonist_path} &

for bg_cores in 0 6; do

    all_pids=()
    config=$1-app$app_cores-bg$bg_cores-antagonist

    function cleanup() {
        for pid in "${all_pids[@]}"; do
            kill -9 $pid > /dev/null 2>&1;
        done;
        killall perf > /dev/null 2>&1;
        killall python3 > /dev/null 2>&1;
        killall stream > /dev/null 2>&1;
        killall bpftrace > /dev/null 2>&1;
        rmmod memeater.ko > /dev/null 2>&1;
        rmmod kswapdrst.ko > /dev/null 2>&1;
        rmmod colloid-mon.ko > /dev/null 2>&1;
        $scripts_path/disable_thp.sh
        echo "Cleaned up";
    }

    trap cleanup EXIT
    cleanup

    # Shrink Local Memory 
    sudo rmmod memeater.ko > /dev/null 2>&1;
    if [ -n "${ENABLE_THP}" ]; then
        echo "Loading memeater + THP"; 
        sudo insmod $memeater_path/memeater.ko sizeMiB=$(numastat -m | grep MemFree | awk -v nidx=$local_numa -v sz=$local_size '{print int($(2+nidx)-sz)}') PGSIZE=2097152 PGORDER=9;

    else
        echo "Loading memeater";
        sudo insmod $memeater_path/memeater.ko sizeMiB=$(numastat -m | grep MemFree | awk -v nidx=$local_numa -v sz=$local_size '{print int($(2+nidx)-sz)}');
    fi 

    sudo sync;
    echo 3 | sudo tee /proc/sys/vm/drop_caches

    if [ -n "${ENABLE_THP}" ]; then
        echo "Enabling THP";
        $scripts_path/enable_thp.sh;
    fi

    # Run kswapdrst
    echo "Running kswapdrst"
    sudo insmod $kswapdrst_path/kswapdrst.ko

    # Make sure swap is disabled
    swapoff -a
    echo 1 | sudo tee /sys/kernel/mm/numa/demotion_enabled
    echo 2 | sudo tee /proc/sys/kernel/numa_balancing

    mio_opts=( $MIO_STATS )
    if [ $bg_cores -gt 0 ]; then
        echo "Running bg traffic on $bg_cores"
        PYTHONPATH=$PYTHONPATH:$mio_path python3 -m mio $config-mio --ant_cpus $bg_core_list --ant_num_cores $bg_cores --ant_mem_numa 1 --ant stream --ant_writefrac 50 --ant_inst_size 64 --ant_duration 10000 "${mio_opts[@]}" &
        pid_mio=$!;
        all_pids+=($pid_mio);
        sleep 7;
    fi

    echo "Local mem size"
    echo $(numastat -m | grep MemFree)
    echo $(numastat -m | grep MemFree) > $stats_path/$config.memfree.txt

    # BPFTRACE
    sudo bpftrace -e 'tracepoint:raw_syscalls:sys_enter { @[comm] = count(); }' > $record_path/bpftrace.txt &
    pid_bpftrace=$!;
    all_pids+=($pid_bpftrace);

    cat /proc/vmstat > $stats_path/$config.before_vmstat.txt

    # SAR
    sar_logfile=$stats_path/$config.sar.txt
    sar -u -P ALL 1 > $sar_logfile 2>&1 &
    pid_sar=$!;
    all_pids+=($pid_sar);

    # Run Latency Benchmark
    echo "Running $config"
    numactl --membind=$local_numa -C $app_core_list $bandwidth_bm_path --threads 15 --duration 120 --warmup 30 --dist zipf > $stats_path/$config.app.txt 2> $stats_path/$config.stderr.txt &
    pid_app=$!;
    all_pids+=($pid_app);

    # Record vm stats for duration
    rm -f $stats_path/$config.vmstat.txt

    if [ $duration -gt 0 ]; then
        for i in $(seq 1 1 $duration); do
            grep -E "pgdemote|pgpromote|pgmigrate|thp_migration" /proc/vmstat >> $stats_path/$config.vmstat.txt
            sleep 1;
        done;
    else
        while is_process_alive $pid_app; do
            grep -E "pgdemote|pgpromote|pgmigrate|thp_migration" /proc/vmstat >> $stats_path/$config.vmstat.txt
            sleep 1;
        done;
    fi

    if [ $duration -gt 0 ]; then
        kill $pid_app > /dev/null 2>&1;
        while is_process_alive $pid_app; do
            sleep 1;
        done;
    fi

    # Stop sar monitoring
    kill $pid_sar > /dev/null 2>&1;
    while is_process_alive $pid_sar; do
        sleep 1;
    done;
    killall sar > /dev/null 2>&1;

    cat /proc/vmstat > $stats_path/$config.after_vmstat.txt

    # Stop bpftrace monitoring
    killall bpftrace > /dev/null 2>&1;
    while is_process_alive $pid_bpftrace; do
        sleep 1;
    done;

    if [ $bg_cores -gt 0 ] || [ "${#mio_opts[@]}" -gt 0 ]; then
        kill $pid_mio > /dev/null 2>&1;
        while is_process_alive $pid_mio; do
                sleep 1;
        done;
        killall python3 > /dev/null 2>&1;
        killall stream > /dev/null 2>&1;
    fi

    rmmod kswapdrst.ko > /dev/null 2>&1;
    rmmod memeater.ko > /dev/null 2>&1;

    $scripts_path/disable_thp.sh;
    echo "Done";

done
