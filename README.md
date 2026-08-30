# nestdaq-rawdata-sorter

Utilities for NestDAQ FileSink raw data.

## sort_stf_by_femid

The program automatically detects input raw-data files under `INPUT_ROOT_DIR`, groups SubTimeFrames by `femId`, sorts them by `femId` and TimeFrame ID, and writes output files in ascending `femId` order.

STF contents are copied byte-for-byte. Heartbeat headers, heartbeat frame delimiters, and payload data are not rewritten.

The program also checks:

- 24-bit HBF number continuity in sorted output order
- HBF start alignment and stop spread
- total input and output byte counts

Usage:

```bash
./sort_stf_by_femid RUNFILE [INPUT_ROOT_DIR] [OUTPUT_ROOT_DIR]
```

Default input layout:

```text
rawdata/00/RUNFILE
rawdata/01/RUNFILE
...
```

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

## extract_first_seconds

Creates a new NestDAQ FileSink raw-data file containing only the beginning of an input raw-data file. The default interval is 10 seconds.

The cut is made at SubTimeFrame (STF) boundaries. STF contents are copied byte-for-byte and are never truncated. The first STF timestamp (`timeSec` + `timeUSec`) is treated as t=0, and all complete STFs whose start time is before the requested limit are copied. The output FileSink header and trailer `stopUnixtime` fields are updated to the last copied STF second.

Usage:

```bash
./extract_first_seconds INPUT.dat OUTPUT.dat [SECONDS]
```

Examples:

```bash
./extract_first_seconds run000020.dat run000020_first10s.dat
./extract_first_seconds run000020.dat run000020_first5s.dat 5
```

Because the cut is made only between STFs, the resulting interval is approximately the requested duration rather than an exact byte-level 10.000000-second cut.

## Build

```bash
make
```

This builds both utilities:

```text
sort_stf_by_femid
extract_first_seconds
```

The default compiler flags are equivalent to:

```bash
g++ -O3 -DNDEBUG -std=c++17 -Wall -Wextra -Wpedantic
```

Help:

```bash
./sort_stf_by_femid --help
./extract_first_seconds --help
```
