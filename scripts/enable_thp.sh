#!/bin/bash

echo "always" | sudo tee /sys/kernel/mm/transparent_hugepage/enabled
echo "always" | sudo tee /sys/kernel/mm/transparent_hugepage/defrag
echo 100 | sudo tee /proc/sys/vm/watermark_scale_factor
