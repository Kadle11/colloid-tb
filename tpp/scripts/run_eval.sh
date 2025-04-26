#!/bin/bash

COLLOID_HOME='/proj/prismgt-PG0/vrao79/colloid-tb/'
SCRIPTS_PATH=$COLLOID_HOME/scripts
GUPS_PATH=$COLLOID_HOME/colloid/apps/gups

APP_CORES=36
PREFIX="tpp"
DURATION=600

# TPP
echo "Running TPP"
for b in 0 8 16 24;
    do MIO_STATS="--stats_membw --ant_prestart_duration 570" $scripts_path/linux.sh $prefix-gups64-rw-app$i-bg$b $duration $i $b -- $gups_path/gups64-rw $i; 
done;

# TPP + colloid
echo "Running TPP + colloid"
for b in 0 8 16 24;
    do MIO_STATS="--stats_membw --ant_prestart_duration 570" $scripts_path/linux-colloid.sh $prefix-colloid-gups64-rw-app$i-bg$b $duration $i $b -- $gups_path/gups64-rw $i; 
done;

echo "Eval 1 Done"

BG_CORES=15 # Background traffic cores (0x => 0, 1x => 5, 2x => 10, 3x => 15)
function resetme() {
    wrmsr -p 0 0x620 0x818; # Resent uncore frequency to default incase we exit in the middle
}

trap resetme EXIT

# TPP
echo "Running TPP"
for u in 0x70e 0x50a 0x408; do 
    wrmsr -p 0 0x620 $u; 
    echo "uncore freq register: $(sudo rdmsr -p 0 0x620)"; 
    for b in $bg_cores; do 
        $scripts_path/linux.sh tpp-unc$u-gups64-rw-app$i-bg$b $duration $i $b -- $gups_path/gups64-rw $i; 
    done; 
done; 
wrmsr -p 0 0x620 0x818;

# TPP + colloid
echo "Running TPP + colloid"
for u in 0x70e 0x50a 0x408; do 
    wrmsr -p 0 0x620 $u; 
    echo "uncore freq register: $(sudo rdmsr -p 0 0x620)"; 
    for b in $bg_cores; do 
        $scripts_path/linux-colloid.sh tpp-colloid-unc$u-gups64-rw-app$i-bg$b $duration $i $b -- $gups_path/gups64-rw $i; 
    done; 
done; 
wrmsr -p 0 0x620 0x818;

echo "Eval 2 Done"

source $scripts_path/config.sh

for b in 0 5 10 15; do
    start_time=$(date +%s)
    DRAMSIZE=4404019200 $scripts_path/linux.sh tpp-gapbs-twitter-1to2-app15-bg$b 0 15 $b -- taskset -c 1,3,5,7,9,11,13,15,17,19,21,23,25,27,29 $gapbs_path/pr -f $gapbs_path/benchmark/graphs/twitter.sg -i1000 -t1e-4 -n20;
    end_time=$(date +%s)
    elapsed_time=$(($end_time - $start_time))
    echo "Took $elapsed_time seconds" >> $stats_path/tpp-gapbs-twitter-1to2-app15-bg$b.time.txt
done;

for b in 0 5 10 15; do
    start_time=$(date +%s)
    DRAMSIZE=4404019200 $scripts_path/linux-colloid.sh tpp-colloid-gapbs-twitter-1to2-app15-bg$b 0 15 $b -- taskset -c 1,3,5,7,9,11,13,15,17,19,21,23,25,27,29 $gapbs_path/pr -f $gapbs_path/benchmark/graphs/twitter.sg -i1000 -t1e-4 -n20; 
    end_time=$(date +%s)
    elapsed_time=$(($end_time - $start_time))
    echo "Took $elapsed_time seconds" >> $stats_path/tpp-colloid-gapbs-twitter-1to2-app15-bg$b.time.txt
done;

echo "Eval 3 Done"
