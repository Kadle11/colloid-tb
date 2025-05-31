#!/bin/bash
# This script is used to inject cross NUMA memory accesses in a given binary.

ARCH=hsw
mio_path=/proj/prismgt-PG0/vrao79/understanding-the-host-network

icx_all_core_list="1,3,5,7,9,11,13,15,17,19,21,23,25,27,29,31,33,35,37,39,41,43,45,47,49,51,53,55,57,59,61,63,65,67,69,71"
hsw_all_core_list="0,2,4,6,8,10,12,14,16,18,20,22,24,26,28"

MIO_STATS="--stats_colloid_mbm --stats_colloid_wait 570"

ant_cores=$1

# Define BG_CORES based on Input Platform (ICX or HSW)
app_cores=16
all_core_list=""
if [[ $ARCH == *"icx"* ]]; then
    all_core_list=$icx_all_core_list
else
    all_core_list=$hsw_all_core_list
fi
ant_core_list=$(echo "$all_core_list" | cut -d ',' -f $((app_cores))-)
echo "Background core list: $ant_core_list"

PYTHONPATH=$PYTHONPATH:$mio_path python3 -m mio cn-traffic-mio --ant_cpus $ant_core_list --ant_num_cores $ant_cores --ant_mem_numa 0 --ant stream --ant_inst_size 64 --ant_duration 1000000
