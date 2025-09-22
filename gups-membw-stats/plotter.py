import sys

import matplotlib.pyplot as plt

def main():
    if len(sys.argv) < 2:
        print("Usage: python plotter.py throughput.txt")
        sys.exit(1)

    input_file = sys.argv[1]
    prefix = input_file.split(".gups.txt")[0]
    output_file = f"{prefix}-thpt.png"

    times = []
    throughputs = []

    with open(input_file, 'r') as f:
        for idx, line in enumerate(f, 1):
            line = line.strip()
            if line and line[0].isdigit() and line[0] != '0':
                parts = line.split()
                try:
                    thpt = float(parts[0])
                    times.append(idx)
                    throughputs.append(thpt)
                except ValueError:
                    continue

    plt.figure(figsize=(10, 5))
    plt.title("Throughput Over Time (1s interval)")
    plt.xlabel("Time (s)")
    plt.ylabel("Throughput (ops/s)")
    plt.grid(True)
    plt.plot(times, throughputs, marker='o', label='ops/s')
    plt.legend()
    plt.tight_layout()
    plt.savefig(output_file)
    print(f"Plot saved to {output_file}")

if __name__ == "__main__":
    main()