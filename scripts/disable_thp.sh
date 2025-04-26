#!/bin/bash

echo "madvise" | sudo tee /sys/kernel/mm/transparent_hugepage/enabled
echo "madvise" | sudo tee /sys/kernel/mm/transparent_hugepage/defrag
echo 10 | sudo tee /proc/sys/vm/watermark_scale_factor