#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#pragma pack(push, 4)
struct FileHeader {
    uint64_t magic;
    uint32_t length;
    uint16_t hLength;
    uint16_t type;
    uint64_t fairMQDeviceType;
    uint64_t runNumber;
    int64_t startUnixtime;
    int64_t stopUnixtime;
    char comments[256];
};

struct STFHeader {
    uint64_t magic;
    uint32_t length;
    uint16_t hLength;
    uint16_t type;
    uint32_t timeFrameId;
    uint32_t femType;
    uint32_t femId;
    uint32_t numMessages;
    uint64_t timeSec;
    uint64_t timeUSec;
};

struct FileTrailer {
    uint64_t magic;
    uint32_t length;
    uint16_t hLength;
    uint16_t type;
    uint64_t fairMQDeviceType;
    uint64_t runNumber;
    int64_t startUnixtime;
    int64_t stopUnixtime;
    char comments[256];
};
#pragma pack(pop)

static_assert(sizeof(FileHeader) == 304);
static_assert(sizeof(STFHeader) == 48);
static_assert(sizeof(FileTrailer) == 304);

constexpr uint64_t FILE_MAGIC = 0x004b4e53454c4946ULL; // FILESNK\0
constexpr uint64_t STF_MAGIC  = 0x00454d4954425553ULL; // SUBTIME\0
constexpr uint64_t TRL_MAGIC  = 0x004c5254454c4946ULL; // FILETRL\0

constexpr uint8_t HEARTBEAT_HEAD = 0x1c;
constexpr uint32_t HBF_MASK = 0x00ffffffu;
constexpr long double HBF_PERIOD_SEC = 524.288e-6L;
constexpr int64_t HBF_START_DELAY_SEC = 1;
constexpr int64_t HBF_STOP_DELAY_SEC = 1;

static bool read_exact(std::ifstream& f, void* p, std::streamsize n) {
    f.read(reinterpret_cast<char*>(p), n);
    return f.gcount() == n;
}

static bool copy_exact(std::ifstream& in, std::ofstream& out, uint64_t n) {
    constexpr size_t BUFSIZE = 1024 * 1024;
    std::vector<char> buf(BUFSIZE);
    while (n > 0) {
        const auto chunk = static_cast<std::streamsize>(n < BUFSIZE ? n : BUFSIZE);
        if (!read_exact(in, buf.data(), chunk)) {
            return false;
        }
        out.write(buf.data(), chunk);
        if (!out) {
            return false;
        }
        n -= static_cast<uint64_t>(chunk);
    }
    return true;
}

static long double stf_time_us(const STFHeader& sh) {
    return static_cast<long double>(sh.timeSec) * 1000000.0L
         + static_cast<long double>(sh.timeUSec);
}

static uint32_t hbf_distance(uint32_t hb, uint32_t anchor) {
    return (hb - anchor) & HBF_MASK;
}

struct HbfRange {
    bool seen = false;
    uint32_t first = 0;
    uint32_t last = 0;
};

static bool scan_hbf_in_stf(std::ifstream& in,
                            std::streampos stf_pos,
                            const STFHeader& sh,
                            HbfRange& range) {
    const uint64_t payload_bytes = sh.length - sizeof(STFHeader);
    const uint64_t nwords = payload_bytes / sizeof(uint64_t);

    in.clear();
    in.seekg(stf_pos + static_cast<std::streamoff>(sizeof(STFHeader)));
    if (!in) {
        return false;
    }

    for (uint64_t i = 0; i < nwords; ++i) {
        uint64_t word = 0;
        if (!read_exact(in, &word, sizeof(word))) {
            return false;
        }

        const uint8_t head = static_cast<uint8_t>((word >> 58) & 0x3f);
        if (head != HEARTBEAT_HEAD) {
            continue;
        }

        const uint32_t hb = static_cast<uint32_t>(word & HBF_MASK);
        if (!range.seen) {
            range.seen = true;
            range.first = hb;
        }
        range.last = hb;
    }

    return true;
}

static void print_help(const char* prog) {
    std::cout
        << "Usage:\n"
        << "  " << prog << " INPUT.dat OUTPUT.dat [SECONDS]\n"
        << "  " << prog << " -h | --help\n\n"
        << "Create a new NestDAQ FileSink raw-data file containing the first\n"
        << "approximately SECONDS seconds of data (default: 10.0 s).\n\n"
        << "The cut is made only at SubTimeFrame (STF) boundaries, so STF contents\n"
        << "are copied byte-for-byte and are never truncated. The first STF timestamp\n"
        << "is used as t=0 for selecting STFs.\n\n"
        << "For the output FileSink header/trailer timestamps, HBF timing is used:\n"
        << "  HBF start ~= startUnixtime + 1 s\n"
        << "  HBF period = 524.288 us\n"
        << "  stopUnixtime = one second after the end of the last copied HBF\n"
        << "The 24-bit HBF counter wrap 0xffffff -> 0x000000 is handled.\n\n"
        << "Example:\n"
        << "  " << prog << " run000020.dat run000020_first10s.dat\n"
        << "  " << prog << " run000020.dat run000020_first5s.dat 5\n";
}

int main(int argc, char** argv) {
    if (argc == 2 && (std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help")) {
        print_help(argv[0]);
        return 0;
    }
    if (argc < 3 || argc > 4) {
        print_help(argv[0]);
        return argc == 1 ? 0 : 1;
    }

    const std::string input_path = argv[1];
    const std::string output_path = argv[2];
    if (input_path == output_path) {
        std::cerr << "ERROR: input and output files must differ.\n";
        return 2;
    }

    double seconds = 10.0;
    if (argc == 4) {
        char* end = nullptr;
        seconds = std::strtod(argv[3], &end);
        if (!end || *end != '\0' || !(seconds > 0.0)) {
            std::cerr << "ERROR: SECONDS must be a positive number.\n";
            return 2;
        }
    }

    std::ifstream in(input_path, std::ios::binary);
    if (!in) {
        std::cerr << "ERROR: cannot open input: " << input_path << "\n";
        return 3;
    }

    FileHeader fh{};
    if (!read_exact(in, &fh, sizeof(fh)) || fh.magic != FILE_MAGIC) {
        std::cerr << "ERROR: invalid NestDAQ FileSink header.\n";
        return 4;
    }

    const auto data_begin = in.tellg();
    long double first_us = -1.0L;
    long double last_us = -1.0L;
    uint64_t copied_stfs = 0;
    uint64_t copied_stf_bytes = 0;
    HbfRange hbf_range;

    while (true) {
        const auto pos = in.tellg();
        if (pos < 0) {
            std::cerr << "ERROR: invalid input stream position.\n";
            return 5;
        }

        uint64_t magic = 0;
        if (!read_exact(in, &magic, sizeof(magic))) {
            std::cerr << "ERROR: input ended before FileTrailer.\n";
            return 5;
        }
        in.clear();
        in.seekg(pos);

        if (magic == TRL_MAGIC) {
            break;
        }
        if (magic != STF_MAGIC) {
            std::cerr << "ERROR: unexpected magic at byte " << static_cast<uint64_t>(pos) << ".\n";
            return 5;
        }

        STFHeader sh{};
        if (!read_exact(in, &sh, sizeof(sh)) || sh.length < sizeof(sh)) {
            std::cerr << "ERROR: invalid STF at byte " << static_cast<uint64_t>(pos) << ".\n";
            return 5;
        }
        if (sh.timeUSec >= 1000000ULL) {
            std::cerr << "ERROR: invalid STF timeUSec=" << sh.timeUSec
                      << " at byte " << static_cast<uint64_t>(pos) << ".\n";
            return 5;
        }

        const long double t_us = stf_time_us(sh);
        if (first_us < 0.0L) {
            first_us = t_us;
        }
        const long double elapsed_us = t_us - first_us;
        if (elapsed_us < 0.0L) {
            std::cerr << "ERROR: STF timestamps go backwards at byte "
                      << static_cast<uint64_t>(pos) << ".\n";
            return 5;
        }
        if (elapsed_us >= static_cast<long double>(seconds) * 1000000.0L) {
            break;
        }

        if (!scan_hbf_in_stf(in, pos, sh, hbf_range)) {
            std::cerr << "ERROR: failed to scan HBF words in STF at byte "
                      << static_cast<uint64_t>(pos) << ".\n";
            return 5;
        }

        last_us = t_us;
        ++copied_stfs;
        copied_stf_bytes += sh.length;

        in.clear();
        in.seekg(pos + static_cast<std::streamoff>(sh.length));
        if (!in) {
            std::cerr << "ERROR: STF extends beyond input file.\n";
            return 5;
        }
    }

    if (copied_stfs == 0 || first_us < 0.0L || last_us < 0.0L) {
        std::cerr << "ERROR: no STF found in requested interval.\n";
        return 6;
    }
    if (!hbf_range.seen) {
        std::cerr << "ERROR: no HBF header found in copied STF data.\n";
        return 6;
    }

    FileTrailer out_tr{};
    in.clear();
    in.seekg(data_begin);
    while (true) {
        const auto pos = in.tellg();
        uint64_t magic = 0;
        if (!read_exact(in, &magic, sizeof(magic))) {
            std::cerr << "ERROR: cannot locate FileTrailer.\n";
            return 5;
        }
        in.clear();
        in.seekg(pos);
        if (magic == TRL_MAGIC) {
            if (!read_exact(in, &out_tr, sizeof(out_tr))) {
                std::cerr << "ERROR: truncated FileTrailer.\n";
                return 5;
            }
            break;
        }
        STFHeader sh{};
        if (!read_exact(in, &sh, sizeof(sh)) || sh.magic != STF_MAGIC || sh.length < sizeof(sh)) {
            std::cerr << "ERROR: invalid STF while locating FileTrailer.\n";
            return 5;
        }
        in.seekg(pos + static_cast<std::streamoff>(sh.length));
    }

    const uint64_t hbf_count = static_cast<uint64_t>(hbf_distance(hbf_range.last, hbf_range.first)) + 1ULL;
    const long double hbf_duration_sec = static_cast<long double>(hbf_count) * HBF_PERIOD_SEC;

    // FileSink stores Unix time as whole seconds. HBF acquisition starts about
    // one second after startUnixtime. The requested stop time is one second
    // after the last copied HBF has finished. Fractional seconds are discarded
    // in the same way as time_t/Unix-second storage.
    const int64_t shortened_stop =
        fh.startUnixtime
        + HBF_START_DELAY_SEC
        + static_cast<int64_t>(hbf_duration_sec)
        + HBF_STOP_DELAY_SEC;

    fh.stopUnixtime = shortened_stop;
    out_tr.startUnixtime = fh.startUnixtime;
    out_tr.stopUnixtime = shortened_stop;

    std::ofstream out(output_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::cerr << "ERROR: cannot create output: " << output_path << "\n";
        return 7;
    }
    out.write(reinterpret_cast<const char*>(&fh), sizeof(fh));
    if (!out) {
        std::cerr << "ERROR: cannot write output header.\n";
        return 7;
    }

    in.clear();
    in.seekg(data_begin);
    if (!copy_exact(in, out, copied_stf_bytes)) {
        std::cerr << "ERROR: failed while copying STF data.\n";
        return 7;
    }
    out.write(reinterpret_cast<const char*>(&out_tr), sizeof(out_tr));
    if (!out) {
        std::cerr << "ERROR: cannot write output trailer.\n";
        return 7;
    }
    out.close();

    const long double actual_stf = (last_us - first_us) / 1000000.0L;
    std::cout << "Created: " << output_path << "\n"
              << "Requested interval : " << std::fixed << std::setprecision(6)
              << seconds << " s\n"
              << "Last STF start     : " << static_cast<double>(actual_stf) << " s from first STF\n"
              << "First HBF          : 0x" << std::hex << std::setw(6) << std::setfill('0')
              << hbf_range.first << std::dec << std::setfill(' ') << "\n"
              << "Last HBF           : 0x" << std::hex << std::setw(6) << std::setfill('0')
              << hbf_range.last << std::dec << std::setfill(' ') << "\n"
              << "HBF count/span     : " << hbf_count << "\n"
              << "HBF duration       : " << static_cast<double>(hbf_duration_sec) << " s\n"
              << "Copied STF count   : " << copied_stfs << "\n"
              << "Copied STF bytes   : " << copied_stf_bytes << "\n"
              << "Output bytes       : " << (sizeof(FileHeader) + copied_stf_bytes + sizeof(FileTrailer)) << "\n"
              << "startUnixtime      : " << fh.startUnixtime << "\n"
              << "HBF nominal start  : " << (fh.startUnixtime + HBF_START_DELAY_SEC) << "\n"
              << "New stopUnixtime   : " << shortened_stop << "\n";

    return 0;
}
