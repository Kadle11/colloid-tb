#!/bin/bash

COLLOID_HOME=/proj/prismgt-PG0/vrao79/colloid-tb
METRICS_DIR="$COLLOID_HOME/ycsb-eval"

OUTPUT_FILE="ycsb_summary.csv"
HEADER="Filename,Throughput(ops/sec),AverageLatency(us),MinLatency(us),MaxLatency(us),50thPercentileLatency(us),95thPercentileLatency(us),99thPercentileLatency(us)"

echo "$HEADER" > "$OUTPUT_FILE"

for FILE in $METRICS_DIR/*.app.txt; do
  # Initialize row with filename
  ROW="$FILE"

  # Extract throughput
  THROUGHPUT=$(grep "^\[OVERALL\].*Throughput" "$FILE" | awk -F, '{print $3}')
  ROW="$ROW,$THROUGHPUT"

  # Extract READ latencies
  for METRIC in AverageLatency MinLatency MaxLatency 50thPercentileLatency 95thPercentileLatency 99thPercentileLatency; do
    VALUE=$(grep "^\[READ\], *$METRIC" "$FILE" | awk -F, '{print $3}')
    ROW="$ROW,$VALUE"
  done

  echo "$ROW" >> "$OUTPUT_FILE"
done

echo "Metrics written to $OUTPUT_FILE"
