#!/bin/bash

set -x

COLLOID_HOME='/proj/prismgt-PG0/vrao79/colloid-tb'
mio_path=/proj/prismgt-PG0/vrao79/understanding-the-host-network

config="colloidmt-baseline-gupsrw"
gups_path=$COLLOID_HOME/apps/gups
record_path=$COLLOID_HOME/colloid-stats
stats_path=$COLLOID_HOME/membw-eval

config="colloidmt-baseline-gupsrw"
gups_workload="gups-rw"
gups_cores=4
stream_num_cores=4
stream_core_list="19,23,27,31"
duration=120

function cleanup() {
    killall gups-r;
    killall gups-rw;
    killall record_stats;
    killall stream;
    killall python3;
    echo "Cleaned up";
}

trap cleanup EXIT

cleanup;

# Run GUPS in isolation
echo "Running $config-iso"
$gups_path/$gups_workload $gups_cores > $stats_path/$config-iso.gups.txt 2>&1 &
pid_gups=$!;
taskset -c 0 $record_path/record_stats > $stats_path/$config-iso.stats.txt 2>&1 &
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

# Run GUPS with background traffic
echo "Running $config-bg"
PYTHONPATH=$PYTHONPATH:$mio_path python3 -m mio $config-bg --ant_cpus $stream_core_list --ant_num_cores $stream_num_cores --ant_mem_numa 1 --ant stream --ant_writefrac 50 --ant_inst_size 64 --ant_duration 10000 &
pid_mio=$!;
sleep 10;
$gups_path/$gups_workload $gups_cores > $stats_path/$config-bg.gups.txt 2>&1 &
pid_gups=$!;
taskset -c 0 $record_path/record_stats > $stats_path/$config-bg.stats.txt 2>&1 &
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
killall python3;
while kill -0 $pid_mio; do
    sleep 1;
done;

echo "Done";
