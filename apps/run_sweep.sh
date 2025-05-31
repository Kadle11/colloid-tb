#!/bin/bash

APPS_DIR=$(pwd)

# Run GAPBS Benchmark
pushd $APPS_DIR/gapbs
export OMP_NUM_THREADS=4
sudo ../sensitivity_sweep.sh bc-google_plus 0 4 1024 "./bc -f ./graphs/google_plus.sg"; 
sudo ../sensitivity_sweep.sh sssp-google_plus 0 4 1024 "./sssp -f ./graphs/google_plus.wsg";
sudo ../sensitivity_sweep.sh bfs-twitter7 0 4 1024 "./bfs -f ./graphs/twitterU.sg";
sudo ../sensitivity_sweep.sh tc-twitter7 0 4 1024 "./tc -f ./graphs/twitterU.sg";
sudo ../sensitivity_sweep.sh pr-twitter7 0 4 1024 "./pr -f ./graphs/twitterU.sg";
popd

# Run Redis + YCSB-A
pushd $APPS_DIR/YCSB
redis-cli FLUSHALL
sudo ../sensitivity_sweep.sh redis-8GB 0 4 1024 "./bin/ycsb run redis -s -P ./workloads/workloada -p "redis.host=127.0.0.1" -threads 4"  " ./bin/ycsb load redis -s -P ./workloads/workloada -p "redis.host=127.0.0.1" -threads 16"
popd

# # Run Memory Benchmark
# pushd $APPS_DIR/MemoryBenchmark
# sudo ../sensitivity_sweep.sh kv-32GB-zipf1_2-sparse 0 16 8192 "./build/kv_benchmark --num_pages 8000000 --queries 160000000 -t 16 -z 1.2 -d zipfian -s 0.1";
# sudo ../sensitivity_sweep.sh kv-32GB-zipf1_2-dense 0 16 8192 "./build/kv_benchmark --num_pages 8000000 --queries 160000000 -t 16 -z 1.2 -d zipfian -s 0.9";
# popd
