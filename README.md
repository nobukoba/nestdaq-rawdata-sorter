# nestdaq-rawdata-sorter

Utility for sorting NestDAQ FileSink raw data produced with auto sub-channel enabled.

The program automatically detects input raw-data files under `INPUT_ROOT_DIR`, groups SubTimeFrames by `femId`, sorts them by `femId` and TimeFrame ID, and writes output files in ascending `femId` order.

STF contents are copied byte-for-byte. Heartbeat headers, heartbeat frame delimiters, and payload data are not rewritten.

The program also checks:

- 24-bit HBF number continuity in sorted output order
- HBF start alignment and stop spread
- total input and output byte counts

## Build

```bash
make
```

or directly:

```bash
g++ -O3 -DNDEBUG -std=c++17 -Wall -Wextra -Wpedantic sort_stf_by_femid.cc -o sort_stf_by_femid
```

## Usage

```bash
./sort_stf_by_femid RUNFILE [INPUT_ROOT_DIR] [OUTPUT_ROOT_DIR]
```

Help:

```bash
./sort_stf_by_femid
./sort_stf_by_femid -h
./sort_stf_by_femid --help
```

Default input layout:

```text
rawdata/00/RUNFILE
rawdata/01/RUNFILE
...
```

All matching subdirectories are detected automatically.

Default output layout:

```text
rawdata_sorted/00/RUNFILE   lowest femId
rawdata_sorted/01/RUNFILE
...                         femId ascending
```

Example:

```bash
./sort_stf_by_femid run000020.dat
```

or:

```bash
./sort_stf_by_femid run000020.dat rawdata rawdata_sorted
```

During processing, scan/copy progress and throughput are displayed.
