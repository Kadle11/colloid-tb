#!/bin/bash

scripts_path="${BASH_SOURCE%/*}/../../scripts"
ycsb_path="${BASH_SOURCE%/*}/../../apps/YCSB"
duration=600
i=16 # no. of app cores

hotness=$1
zipFian=$2

prefix="tpp"
if [ -n "$PREFIX" ]; then
    prefix="$PREFIX"
fi

# TPP
echo "Running TPP"
for b in 0 5 10 15;
    do MIO_STATS="--stats_colloid_mbm --stats_colloid_wait 570" $scripts_path/linux.sh $prefix-"$zipFian"ZipF-"$hotness"hot-ycsbk-app$i-bg$b $duration $i $b -- sudo likwid-pin -C 35,37,39,41,43,45,47,49,51,53,55,57,59,61,63,65,67,69 $ycsb_path/bin/ycsb run redis -s -P $ycsb_path/workloads/workloadk -p "redis.host=127.0.0.1" -threads 16; 
    redis-cli FLUSHALL
done

# TPP + colloid
echo "Running TPP+colloid"
for b in 0 5 10 15;
    do MIO_STATS="--stats_colloid_mbm --stats_colloid_wait 570" $scripts_path/linux-colloid.sh $prefix-colloid-"$zipFian"ZipF-"$hotness"hot-ycsbk-app$i-bg$b $duration $i $b -- sudo likwid-pin -C 35,37,39,41,43,45,47,49,51,53,55,57,59,61,63,65,67,69 $ycsb_path/bin/ycsb run redis -s -P $ycsb_path/workloads/workloadk -p "redis.host=127.0.0.1" -threads 16; 
    redis-cli FLUSHALL
done

