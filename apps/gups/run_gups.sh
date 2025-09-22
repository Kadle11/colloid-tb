#!/bin/bash

# set -x

config=$1

colloid_home='/proj/prismgt-PG0/vrao79/colloid-tb'

gups_path=$colloid_home/apps/gups
mio_path=/proj/prismgt-PG0/vrao79/understanding-the-host-network
record_path=$colloid_home/colloid-stats
stats_path=$colloid_home/gups-membw-stats-tmp
memeater_path=$colloid_home/tpp/memeater
colloidmon_path=$colloid_home/tpp/colloid-mon
kswapdrst_path=$colloid_home/tpp/kswapdrst

local_numa=0
remote_numa=1
local_size=32768

gups_workload=gups64-rw

gups_cores=4
gups_core_list="0,2,28,30"
duration=600
interference_ts=240
stream_num_cores=24
stream_core_list="4,6,8,10,12,14,16,18,20,22,24,26,32,34,36,38,40,42,44,46,48,50,52,54"
hs_step=0.5

mkdir -p $stats_path

mio_opts=( $MIO_STATS )

function cleanup() {
    killall gups-r;
    killall gups-rw;
    killall gups64-rw;
    killall record_stats;
    killall stream;
    killall python3;
    sudo rmmod $memeater_path/memeater.ko;
    sudo rmmod $colloidmon_path/colloid-mon.ko
    echo "Cleaned up";
}

trap cleanup EXIT

cleanup;

# Make sure tiering is disabled
sudo insmod $kswapdrst_path/kswapdrst.ko

sudo sysctl vm.nr_hugepages=0
sudo swapoff -a
echo 0 | sudo tee /sys/kernel/mm/numa/demotion_enabled
echo 0 | sudo tee /proc/sys/kernel/numa_balancing


# Set local memory capacity
sudo insmod $memeater_path/memeater.ko sizeMiB=$(numastat -m | grep MemFree | awk -v nidx=$local_numa -v sz=$local_size -v b=$stream_num_cores '{print int($(2+nidx)-sz-b*512)}');
echo "Local mem size"
echo $(numastat -m | grep MemFree)

# Run GUPS with varying percentage of hot set in local memory
for x in $(seq 0 $hs_step 1); do

    sync; echo 3 | sudo tee /proc/sys/vm/drop_caches;
    sleep 10;

    echo "Running $config-iso-x$x";
    GUPS_DURATION=45 $gups_path/$gups_workload $gups_cores manual $x distribute > $stats_path/$config-iso-x$x.gups.txt 2>&1 &
    pid_gups=$!;
    sleep 60;
    killall $gups_workload;
    while kill -0 $pid_gups; do
        sleep 1;
    done;
done;

echo "Done with isolated runs; Starting TPP runs";

# Enable tiering
echo 1 | sudo tee /sys/kernel/mm/numa/demotion_enabled
echo 2 | sudo tee /proc/sys/kernel/numa_balancing

# Run GUPS with varying percentage of hot set in local memory
for x in $(seq 0 $hs_step 1); do

    sync; echo 3 | sudo tee /proc/sys/vm/drop_caches;
    sleep 10;

    tpp_config="$config-iso-tpp-x$x"
    echo "Running $tpp_config";
    GUPS_DURATION=$duration $gups_path/$gups_workload $gups_cores manual $x distribute reset > $stats_path/$tpp_config.gups.txt &
    pid_gups=$!;

    # record vm stats for duration
    rm -f $stats_path/$tpp_config.vmstat.txt
    if [ $duration -gt 0 ]; then
        for i in $(seq 1 1 $duration); do
            grep -E "pgdemote|pgpromote|pgmigrate|thp_migration" /proc/vmstat >> $stats_path/$tpp_config.vmstat.txt
            sleep 1;
        done;
    else
        while kill -0 $pid_app > /dev/null 2>&1; do
            grep -E "pgdemote|pgpromote|pgmigrate|thp_migration" /proc/vmstat >> $stats_path/$tpp_config.vmstat.txt
            sleep 1;
        done;
    fi

    sleep 10;

    killall $gups_workload;
    while kill -0 $pid_gups; do
        sleep 1;
    done;
done;

echo "Done with TPP runs; Starting Colloid runs";

# Enable Colloid
sudo insmod $colloidmon_path/colloid-mon.ko
echo 6 | sudo tee /proc/sys/kernel/numa_balancing

addr_occ_local=$(cat /proc/kallsyms | grep smoothed_occ_local | awk '{print "0x"$1}')
addr_occ_remote=$(cat /proc/kallsyms | grep smoothed_occ_remote | awk '{print "0x"$1}')
addr_lat_local=$(cat /proc/kallsyms | grep smoothed_lat_local | awk '{print "0x"$1}')
addr_lat_remote=$(cat /proc/kallsyms | grep smoothed_lat_remote | awk '{print "0x"$1}')
addr_inserts_local=$(cat /proc/kallsyms | grep smoothed_inserts_local | awk '{print "0x"$1}')
addr_inserts_remote=$(cat /proc/kallsyms | grep smoothed_inserts_remote | awk '{print "0x"$1}')

# Run GUPS with varying percentage of hot set in local memory
for x in $(seq 0 $hs_step 1); do

    sync; echo 3 | sudo tee /proc/sys/vm/drop_caches;
    sleep 10;

    colloid_config="$config-iso-colloid-x$x"
    echo "Running $colloid_config";
    GUPS_DURATION=$duration $gups_path/$gups_workload $gups_cores manual $x distribute reset > $stats_path/$colloid_config.gups.txt &
    pid_gups=$!;
    bpftrace -e "BEGIN {@start = nsecs;} interval:s:1 {printf(\"%ld, colloid_local_lat_gt_remote: %d, local_lat: %lu, remote_lat: %lu, local_occ: %lu, remote_occ: %lu, local_inserts: %lu, remote_inserts: %lu\n\", (nsecs-@start)/1e9, *kaddr(\"colloid_local_lat_gt_remote\"), *($addr_lat_local), *($addr_lat_remote), *($addr_occ_local), *($addr_occ_remote), *($addr_inserts_local), *($addr_inserts_remote));}" > $stats_path/$colloid_config.mon.txt 2>&1 &
    pid_bpf=$!;
    
    # record vm stats for duration
    rm -f $stats_path/$colloid_config.vmstat.txt
    if [ $duration -gt 0 ]; then
        for i in $(seq 1 1 $duration); do
            grep -E "pgdemote|pgpromote|pgmigrate|thp_migration" /proc/vmstat >> $stats_path/$colloid_config.vmstat.txt
            sleep 1;
        done;
    else
        while kill -0 $pid_app > /dev/null 2>&1; do
            grep -E "pgdemote|pgpromote|pgmigrate|thp_migration" /proc/vmstat >> $stats_path/$colloid_config.vmstat.txt
            sleep 1;
        done;
    fi

    killall bpftrace;
    while kill -0 $pid_bpf; do
        sleep 1;
    done;
    
    sleep 10;

    killall $gups_workload;
    while kill -0 $pid_gups; do
        sleep 1;
    done;

done;

# Run GUPS with varying percentage of hot set in local memory + background traffic
for x in $(seq 0 $hs_step 1); do

    sync; echo 3 | sudo tee /proc/sys/vm/drop_caches;
    sleep 10;
    
    colloid_config="$config-iso-colloid-x$x-bg"
    echo "Running $colloid_config";
    GUPS_DURATION=$duration $gups_path/$gups_workload $gups_cores manual $x distribute reset > $stats_path/$colloid_config.gups.txt &
    pid_gups=$!;
    bpftrace -e "BEGIN {@start = nsecs;} interval:s:1 {printf(\"%ld, colloid_local_lat_gt_remote: %d, local_lat: %lu, remote_lat: %lu, local_occ: %lu, remote_occ: %lu, local_inserts: %lu, remote_inserts: %lu\n\", (nsecs-@start)/1e9, *kaddr(\"colloid_local_lat_gt_remote\"), *($addr_lat_local), *($addr_lat_remote), *($addr_occ_local), *($addr_occ_remote), *($addr_inserts_local), *($addr_inserts_remote));}" > $stats_path/$colloid_config.mon.txt 2>&1 &
    pid_bpf=$!;
    
    # record vm stats for duration
    rm -f $stats_path/$colloid_config.vmstat.txt
    for i in $(seq 1 1 $duration); do
        grep -E "pgdemote|pgpromote|pgmigrate|thp_migration" /proc/vmstat >> $stats_path/$colloid_config.vmstat.txt
        sleep 1;

        if [ $i -eq $interference_ts ] && ( [ $stream_num_cores -gt 0 ] || [ "${#mio_opts[@]}" -gt 0 ] ); then
            pid_mio=-1;
            if [ $stream_num_cores -gt 0 ]; then
                echo "Starting interference at $i seconds";
                PYTHONPATH=$PYTHONPATH:$mio_path python3 -m mio $colloid_config-mio --ant_cpus $stream_core_list --ant_num_cores $stream_num_cores --ant_mem_numa $local_numa --ant stream --ant_writefrac 50 --ant_inst_size 64 --ant_duration 10000 "${mio_opts[@]}" &
                pid_mio=$!;
            elif [ "${#mio_opts[@]}" -gt 0 ]; then
                echo "Starting interference at $i seconds";
                PYTHONPATH=$PYTHONPATH:$mio_path python3 -m mio $colloid_config-mio "${mio_opts[@]}" &
                pid_mio=$!;
            fi
        fi

    done;

    killall bpftrace;
    while kill -0 $pid_bpf; do
        sleep 1;
    done;

    killall python3;
    killall stream-nt;

    while kill -0 $pid_mio > /dev/null 2>&1; do
        sleep 1;
    done;
    
    sleep 10;

    killall $gups_workload;
    while kill -0 $pid_gups; do
        sleep 1;
    done;

done;

sudo rmmod $colloidmon_path/colloid-mon.ko
sudo rmmod $kswapdrst_path/kswapdrst.ko
sudo rmmod $memeater_path/memeater.ko;

echo "Done";
