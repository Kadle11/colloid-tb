#!/bin/bash

set -x

config=$1

colloid_home='/proj/prismgt-PG0/vrao79/colloid-tb'

gups_path=$colloid_home/apps/gups
mio_path=$colloid_home/mio
record_path=$colloid_home/colloid-stats
stats_path=$colloid_home/membw-stats
memeater_path=$colloid_home/tpp/memeater

local_numa=1
local_size=32768

gups_workload=$2
gups_cores=15
# stream_num_cores=$4
# all_core_list="1,3,5,7,9,11,13,15,17,19,21,23,25,27,29,31,33,35,37,39,41,43,45,47,49,51,53,55,57,59"
duration=45

mio_opts=( $MIO_STATS )

function cleanup() {
    killall gups-r;
    killall gups-rw;
    killall gups64-rw;
    killall record_stats;
    killall stream;
    killall python3;
    sudo rmmod $memeater_path/memeater.ko;
    echo "Cleaned up";
}

trap cleanup EXIT

cleanup;

# Make sure tiering is disabled
sudo swapoff -a
echo 0 | sudo tee /sys/kernel/mm/numa/demotion_enabled
echo 0 | sudo tee /proc/sys/kernel/numa_balancing

# Set local memory capacity
sudo insmod $memeater_path/memeater.ko sizeMiB=$(numastat -m | grep MemFree | awk -v nidx=$local_numa -v sz=$local_size -v b=$stream_num_cores '{print int($(2+nidx)-sz-b*512)}');
echo "Local mem size"
echo $(numastat -m | grep MemFree)

# Clear the cores
sudo cset shield -c 3,5,7,9,11,13,15,17,19,21,23,25,27,29,31 -k on

# Run GUPS with varying percentage of hot set in local memory
for x in 0 0.1 0.2 0.3 0.4 0.5 0.6 0.7 0.8 0.9 1; do
    echo "Running $config-iso-x$x";
    GUPS_HUGEPAGES=1 sudo cset shield --exec -- $gups_path/$gups_workload $gups_cores manual $x distribute > $stats_path/$config-iso-x$x.gups.txt 2>&1 &
    pid_gups=$!;
    taskset -c 0 $record_path/record_stats > $stats_path/$config-iso-x$x.stats.txt 2>&1 &
    pid_stats=$!;
    sleep $duration;
    killall record_stats;
    while kill -0 $pid_stats; do
        sleep 1;
    done;
    killall $gups_workload;
    while kill -0 $pid_gups; do
        sleep 1;
    done;
done;


# Run GUPS with varying percentage of hot set in local memory + background traffic
# for x in 0 0.1 0.2 0.3 0.4 0.5 0.6 0.7 0.8 0.9 1; do
# #for x in 1.0; do
#     echo "Running $config-bg-x$x";
#     pid_mio=-1;
#     if [ $stream_num_cores -gt 0 ]; then
#     	PYTHONPATH=$PYTHONPATH:$mio_path python3 -m mio $config-x$x-mio --ant_cpus $stream_core_list --ant_num_cores $stream_num_cores --ant_mem_numa 1 --ant stream --ant_writefrac 50 --ant_inst_size 64 --ant_duration 10000 "${mio_opts[@]}" &
#     	pid_mio=$!;
#     elif [ "${#mio_opts[@]}" -gt 0 ]; then
#         PYTHONPATH=$PYTHONPATH:$mio_path python3 -m mio $config-x$x-mio "${mio_opts[@]}" &
#     	pid_mio=$!;
#     fi
#     sleep 3;

#     echo $(numastat -m | grep MemFree)
#     echo $(numastat -m | grep MemFree) > $stats_path/$config-x$x.memfree.txt

#     numactl --membind 0 $gups_path/$gups_workload $gups_cores manual $x distribute > $stats_path/$config-x$x.app.txt 2>&1 &
#     pid_gups=$!;
#     #taskset -c 0 $record_path/record_stats > $stats_path/$config-bg-x$x.stats.txt 2>&1 &
#     #pid_stats=$!;
#     sleep $duration;
#     #killall record_stats;
#     #while kill -0 $pid_stats; do
#      #   sleep 1;
#     #done;
#     killall $gups_workload;
#     while kill -0 $pid_gups; do
#         sleep 1;
#     done;
#     if [ $stream_num_cores -gt 0 ] || [ "${#mio_opts[@]}" -gt 0 ]; then
#     	killall python3;
#     	killall stream;
# 	sleep 1;
#    fi
# done;

rmmod $memeater_path/memeater.ko;

echo "Done";
