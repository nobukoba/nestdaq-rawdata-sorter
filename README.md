# nestdaq-rawdata-sorter

Utility for sorting NestDAQ FileSink raw data produced with auto sub-channel enabled.

The program reads three input raw-data files, groups SubTimeFrames by `femId`, sorts them by `femId` and TimeFrame ID, and writes three output files in ascending `femId` order.

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
g++ -O3 -DNDEBUG -std=c++17 -Wall -Wextra -Wpedantic sort_rawdata_by_femid.cc -o sort_rawdata_by_femid
```

## Usage

```bash
./sort_rawdata_by_femid RUNFILE [INPUT_ROOT] [OUTPUT_ROOT]
```

Help:

```bash
./sort_rawdata_by_femid
./sort_rawdata_by_femid -h
./sort_rawdata_by_femid --help
```

Default input layout:

```text
rawdata/00/RUNFILE
rawdata/01/RUNFILE
rawdata/02/RUNFILE
```

Default output layout:

```text
rawdata_sorted/00/RUNFILE   lowest femId
rawdata_sorted/01/RUNFILE
rawdata_sorted/02/RUNFILE   highest femId
```

Example:

```bash
./sort_rawdata_by_femid run000020.dat
```

or:

```bash
./sort_rawdata_by_femid run000020.dat rawdata rawdata_sorted
```

During processing, scan/copy progress and throughput are displayed.
