#!/bin/bash

COLLOID_TPP_HOME='/proj/prismgt-PG0/vrao79/colloid-tb/tpp'

# Build kernel modules
cd $COLLOID_TPP_HOME/colloid-mon
make

cd $COLLOID_TPP_HOME/tierinit
make

cd $COLLOID_TPP_HOME/memeater
make

cd $COLLOID_TPP_HOME/kswapdrst
make

cd $COLLOID_TPP_HOME

# Insert the modules
sudo insmod tierinit/tierinit.ko
sudo insmod colloid-mon/colloid-mon.ko

# Enable memory tiering and colloid
swapoff -a # Disable swap
echo 1 | sudo tee /sys/kernel/mm/numa/demotion_enabled # Enable page demotion
echo 6 | sudo tee /proc/sys/kernel/numa_balancing # Enable colloid