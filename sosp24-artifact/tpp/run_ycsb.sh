#!/bin/bash

scripts_path="${BASH_SOURCE%/*}/../../scripts"
ycsb_path="${BASH_SOURCE%/*}/../../apps/YCSB"
duration=1200
i=8 # no. of app cores

hotness=$1
zipFian=$2

prefix="tpp"
if [ -n "$PREFIX" ]; then
    prefix="$PREFIX"
fi

# TPP
echo "Running TPP"
for b in 0 2 4 8;
    do MIO_STATS="--stats_colloid_mbm --stats_colloid_wait 570" $scripts_path/linux.sh $prefix-"$zipFian"ZipF-"$hotness"hot-ycsbk-app$i-bg$b $duration $i $b -- likwid-pin -C 1,3,5,7,9,11,13,15 $ycsb_path/bin/ycsb load redis -s -P $ycsb_path/workloads/workloadk -p "redis.host=127.0.0.1" -threads 8; likwid-pin -C 1,3,5,7,9,11,13,15 bin/ycsb run redis -s -P $ycsb_path/bin/ycsb run redis -s -P $ycsb_path/workloads/workloadk -p "redis.host=127.0.0.1" -threads 8 
    redis-cli FLUSHALL
done

# TPP + colloid
echo "Running TPP+colloid"
for b in 0 2 4 8;
    do MIO_STATS="--stats_colloid_mbm --stats_colloid_wait 570" $scripts_path/linux-colloid.sh $prefix-colloid-"$zipFian"ZipF-"$hotness"hot-ycsbk-app$i-bg$b $duration $i $b -- likwid-pin -C 1,3,5,7,9,11,13,15 $ycsb_path/bin/ycsb load redis -s -P $ycsb_path/workloads/workloadk -p "redis.host=127.0.0.1" -threads 8; likwid-pin -C 1,3,5,7,9,11,13,15 $ycsb_path/bin/ycsb run redis -s -P $ycsb_path/workloads/workloadk -p "redis.host=127.0.0.1" -threads 8;
    redis-cli FLUSHALL
done

