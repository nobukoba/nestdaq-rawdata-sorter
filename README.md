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

The cut is made at SubTimeFrame (STF) boundaries. STF contents are copied byte-for-byte and are never truncated. The first STF timestamp (`timeSec` + `timeUSec`) is treated as t=0, and all complete STFs whose start time is before the requested limit are copied.

For the output FileSink header/trailer Unix times, the HBF timing is used:

- HBF acquisition starts approximately 1 second after `startUnixtime`.
- One HBF is 524.288 us.
- The first and last HBF numbers are read directly from the copied RAW data.
- The HBF counter is treated as a cyclic 24-bit counter, including `0xffffff -> 0x000000` wrap.
- `stopUnixtime` is set to approximately 1 second after the end of the last copied HBF.

Thus, for about 10 seconds of HBF data, `stopUnixtime` is normally about `startUnixtime + 12` seconds.

Usage:

```bash
./extract_first_seconds INPUT.dat OUTPUT.dat [SECONDS]
```

Examples:

```bash
./extract_first_seconds run000020.dat run000020_first10s.dat
./extract_first_seconds run000020.dat run000020_first5s.dat 5
```

The program prints the first/last HBF numbers, HBF span/count, calculated HBF duration, and the resulting `stopUnixtime`.

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
