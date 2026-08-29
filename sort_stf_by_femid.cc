#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

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
constexpr uint64_t STF_MAGIC = 0x00454d4954425553ULL;  // SUBTIME\0
constexpr uint64_t TRL_MAGIC = 0x004c5254454c4946ULL;  // FILETRL\0
constexpr uint8_t HEARTBEAT_HEAD = 0x1c;
constexpr uint32_t HBF_MASK = 0x00ffffffu;
constexpr uint32_t HBF_MODULO = HBF_MASK + 1u;

static uint32_t next_hbf(uint32_t hb) {
    // HBF is a 24-bit cyclic counter:
    // ... 0xfffffe, 0xffffff, 0x000000, 0x000001, ...
    return (hb + 1u) & HBF_MASK;
}

static uint32_t hbf_distance(uint32_t hb, uint32_t anchor) {
    // Forward distance on the 24-bit ring.  Thus, with an anchor near the
    // end of the counter range, 0x000000 naturally follows 0xffffff.
    return (hb - anchor) & HBF_MASK;
}

static uint32_t find_hbf_anchor(std::vector<uint32_t> points) {
    if (points.empty()) {
        return 0;
    }

    std::sort(points.begin(), points.end());
    points.erase(std::unique(points.begin(), points.end()), points.end());

    if (points.size() == 1) {
        return points.front();
    }

    // Choose the first HBF after the largest unused interval on the 24-bit
    // ring.  This rotates the numeric order at the run boundary instead of
    // at 0x000000.  Example:
    //   0xf4.... -> ... -> 0xffffff -> 0x000000 -> ... -> 0x70....
    // remains in exactly that chronological order.
    uint32_t anchor = points.front();
    uint32_t largest_gap = 0;

    for (size_t i = 0; i + 1 < points.size(); ++i) {
        uint32_t gap = points[i + 1] - points[i];
        if (gap > largest_gap) {
            largest_gap = gap;
            anchor = points[i + 1];
        }
    }

    uint32_t wrap_gap = (points.front() - points.back()) & HBF_MASK;
    if (wrap_gap > largest_gap) {
        anchor = points.front();
    }

    return anchor;
}

struct Record {
    uint32_t tfid = 0;
    uint32_t femid = 0;
    uint32_t length = 0;
    size_t input_index = 0;
    uint64_t offset = 0;
    uint64_t serial = 0;
    bool hb_seen = false;
    uint32_t first_hb = 0;
    uint32_t last_hb = 0;
    uint32_t hb_count = 0;
    uint32_t internal_hb_gaps = 0;
    uint32_t hbf_sort_key = std::numeric_limits<uint32_t>::max();
};

struct HbfStats {
    bool seen = false;
    uint32_t first = 0;
    uint32_t last = 0;
    uint64_t count = 0;
    uint64_t discontinuities = 0;
    std::vector<std::pair<uint32_t, uint32_t>> first_gaps;
};

struct Input {
    fs::path path;
    std::ifstream f;
    uint64_t size = 0;
    std::vector<char> header;
    std::vector<char> trailer;
};

static bool read_exact(std::ifstream& f, void* p, std::streamsize n) {
    f.read(reinterpret_cast<char*>(p), n);
    return f.gcount() == n;
}

static uint64_t filesize(std::ifstream& f) {
    auto p = f.tellg();
    f.seekg(0, std::ios::end);
    auto e = f.tellg();
    f.seekg(p);
    return static_cast<uint64_t>(e);
}

static std::string hex24(uint32_t v) {
    std::ostringstream os;
    os << "0x" << std::hex << std::setw(6) << std::setfill('0') << (v & HBF_MASK);
    return os.str();
}

static std::string hex32(uint32_t v) {
    std::ostringstream os;
    os << "0x" << std::hex << std::setw(8) << std::setfill('0') << v;
    return os.str();
}

static std::string ipv4(uint32_t v) {
    std::ostringstream os;
    os << ((v >> 24) & 255) << '.' << ((v >> 16) & 255) << '.'
       << ((v >> 8) & 255) << '.' << (v & 255);
    return os.str();
}

class Progress {
    std::string label;
    uint64_t total;
    int last = -1;
    std::chrono::steady_clock::time_point t0;

public:
    Progress(std::string l, uint64_t t)
        : label(std::move(l)), total(t), t0(std::chrono::steady_clock::now()) {}

    void update(uint64_t done, bool force = false) {
        int p = total ? int((100.0 * done) / total) : 100;
        if (p > 100) {
            p = 100;
        }
        if (!force && p == last) {
            return;
        }

        last = p;
        auto dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        double mb = done / 1e6;
        double rate = dt > 0 ? mb / dt : 0;
        std::cerr << '\r' << label << ": " << std::setw(3) << p << "%  "
                  << std::fixed << std::setprecision(1) << mb << "/" << total / 1e6
                  << " MB  " << rate << " MB/s" << std::flush;
        if (p == 100 || force) {
            std::cerr << "\n";
        }
    }
};

static void print_help(const char* prog) {
    std::cout
        << "Usage:\n"
        << "  " << prog << " RUNFILE [INPUT_ROOT_DIR] [OUTPUT_ROOT_DIR]\n"
        << "  " << prog << " -h | --help\n\n"
        << "Sort NestDAQ FileSink raw data split by auto sub-channel into N\n"
        << "output files grouped by femId (ascending). Input subdirectories are detected\n"
        << "automatically. STF byte contents are copied unchanged. HBF number continuity\n"
        << "and input/output total size are checked. HBF is treated as a cyclic 24-bit\n"
        << "counter, so 0x000000 remains after 0xffffff when a run crosses the wrap.\n\n"
        << "Arguments:\n"
        << "  RUNFILE          Raw-data filename, e.g. run000020.dat\n"
        << "  INPUT_ROOT_DIR   Input root directory  (default: rawdata)\n"
        << "  OUTPUT_ROOT_DIR  Output root directory (default: rawdata_sorted)\n\n"
        << "Default layout:\n"
        << "  input : rawdata/00/RUNFILE\n"
        << "          rawdata/01/RUNFILE\n"
        << "          ... (all matching subdirectories are auto-detected)\n"
        << "  output: rawdata_sorted/00/RUNFILE  (lowest femId)\n"
        << "          rawdata_sorted/01/RUNFILE\n"
        << "          ... (femId ascending)\n\n"
        << "Examples:\n"
        << "  " << prog << " run000020.dat\n"
        << "  " << prog << " run000020.dat rawdata rawdata_sorted\n";
}

int main(int argc, char** argv) {
    if (argc == 1) {
        print_help(argv[0]);
        return 0;
    }
    if (argc == 2 &&
        (std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help")) {
        print_help(argv[0]);
        return 0;
    }
    if (argc < 2 || argc > 4) {
        print_help(argv[0]);
        return 1;
    }

    const std::string runfile = argv[1];
    fs::path inroot = argc >= 3 ? argv[2] : "rawdata";
    fs::path outroot = argc >= 4 ? argv[3] : "rawdata_sorted";

    if (fs::absolute(inroot).lexically_normal() == fs::absolute(outroot).lexically_normal()) {
        std::cerr << "ERROR: input and output roots must differ (refusing in-place overwrite).\n";
        return 2;
    }

    std::vector<fs::path> input_paths;
    if (!fs::exists(inroot) || !fs::is_directory(inroot)) {
        std::cerr << "ERROR: input root is not a directory: " << inroot << "\n";
        return 3;
    }

    for (const auto& entry : fs::directory_iterator(inroot)) {
        if (!entry.is_directory()) {
            continue;
        }
        fs::path p = entry.path() / runfile;
        if (fs::is_regular_file(p)) {
            input_paths.push_back(p);
        }
    }

    std::sort(input_paths.begin(), input_paths.end());
    if (input_paths.empty()) {
        std::cerr << "ERROR: no input files found: " << inroot << "/*/" << runfile << "\n";
        return 3;
    }

    std::vector<Input> in(input_paths.size());
    uint64_t input_total = 0;
    for (size_t i = 0; i < input_paths.size(); ++i) {
        in[i].path = input_paths[i];
        in[i].f.open(in[i].path, std::ios::binary);
        if (!in[i].f) {
            std::cerr << "ERROR: cannot open " << in[i].path << "\n";
            return 3;
        }
        in[i].size = filesize(in[i].f);
        input_total += in[i].size;
        in[i].f.seekg(0);
    }

    std::cout << "Detected " << in.size() << " input file(s):\n";
    for (const auto& x : in) {
        std::cout << "  " << x.path << "\n";
    }

    std::vector<Record> records;
    records.reserve(static_cast<size_t>(input_total / 256));
    std::vector<uint32_t> hbf_points;
    std::set<uint32_t> femids;
    uint64_t serial = 0;
    uint64_t scan_done = 0;
    uint64_t stf_bytes = 0;
    Progress scanprog("Scan + HBF check", input_total);

    for (size_t fi = 0; fi < in.size(); ++fi) {
        auto& x = in[fi];
        FileHeader fh{};

        if (!read_exact(x.f, &fh, sizeof(fh)) || fh.magic != FILE_MAGIC) {
            std::cerr << "\nERROR: invalid FileSink header: " << x.path << "\n";
            return 4;
        }

        x.header.resize(sizeof(FileHeader));
        x.f.clear();
        x.f.seekg(0);
        if (!read_exact(x.f, x.header.data(), static_cast<std::streamsize>(x.header.size()))) {
            std::cerr << "\nERROR: cannot reread FileSink header: " << x.path << "\n";
            return 4;
        }
        scan_done += sizeof(FileHeader);
        scanprog.update(scan_done);

        while (true) {
            auto pos = x.f.tellg();
            if (pos < 0) {
                break;
            }

            uint64_t magic = 0;
            if (!read_exact(x.f, &magic, 8)) {
                break;
            }
            x.f.clear();
            x.f.seekg(pos);

            if (magic == STF_MAGIC) {
                STFHeader sh{};
                if (!read_exact(x.f, &sh, sizeof(sh)) || sh.length < sizeof(sh)) {
                    std::cerr << "\nERROR: bad STF header in " << x.path
                              << " at " << uint64_t(pos) << "\n";
                    return 5;
                }

                uint64_t end = uint64_t(pos) + sh.length;
                if (end > x.size) {
                    std::cerr << "\nERROR: STF beyond EOF in " << x.path << "\n";
                    return 6;
                }

                Record rec;
                rec.tfid = sh.timeFrameId;
                rec.femid = sh.femId;
                rec.length = sh.length;
                rec.input_index = fi;
                rec.offset = uint64_t(pos);
                rec.serial = serial++;

                femids.insert(sh.femId);
                stf_bytes += sh.length;

                uint64_t payload = sh.length - sizeof(sh);
                uint64_t nword = payload / 8;
                uint32_t prev_hb = 0;

                for (uint64_t w = 0; w < nword; ++w) {
                    uint64_t word = 0;
                    if (!read_exact(x.f, &word, 8)) {
                        std::cerr << "\nERROR: truncated STF payload\n";
                        return 7;
                    }

                    uint8_t head = uint8_t((word >> 58) & 0x3f);
                    if (head == HEARTBEAT_HEAD) {
                        uint32_t hb = uint32_t(word & HBF_MASK);
                        if (!rec.hb_seen) {
                            rec.hb_seen = true;
                            rec.first_hb = hb;
                        } else if (hb != next_hbf(prev_hb)) {
                            ++rec.internal_hb_gaps;
                        }
                        prev_hb = hb;
                        rec.last_hb = hb;
                        ++rec.hb_count;
                    }
                }

                if (rec.hb_seen) {
                    hbf_points.push_back(rec.first_hb);
                }
                records.push_back(rec);

                uint64_t rem = payload % 8;
                if (rem) {
                    x.f.seekg(static_cast<std::streamoff>(rem), std::ios::cur);
                }

                scan_done += sh.length;
                scanprog.update(scan_done);
                x.f.clear();
                x.f.seekg(static_cast<std::streamoff>(end));
            } else if (magic == TRL_MAGIC) {
                uint64_t tpos = uint64_t(pos);
                uint64_t tlen = x.size - tpos;
                x.trailer.resize(static_cast<size_t>(tlen));
                x.f.clear();
                x.f.seekg(pos);
                if (!read_exact(x.f, x.trailer.data(), static_cast<std::streamsize>(tlen))) {
                    std::cerr << "\nERROR: bad trailer in " << x.path << "\n";
                    return 8;
                }
                scan_done += tlen;
                scanprog.update(scan_done);
                break;
            } else {
                std::cerr << "\nERROR: unknown magic " << std::hex << magic << std::dec
                          << " in " << x.path << " at " << uint64_t(pos) << "\n";
                return 9;
            }
        }

        if (x.trailer.empty()) {
            std::cerr << "\nERROR: no trailer in " << x.path << "\n";
            return 10;
        }
    }
    scanprog.update(input_total, true);

    if (femids.size() != in.size()) {
        std::cerr << "ERROR: found " << femids.size() << " femIds but " << in.size()
                  << " input files.\n"
                  << "       One FileSink header/trailer is preserved per output, so these counts must match.\n";
        return 11;
    }

    std::vector<uint32_t> order(femids.begin(), femids.end());
    std::map<uint32_t, size_t> outidx;
    for (size_t i = 0; i < order.size(); ++i) {
        outidx[order[i]] = i;
    }

    bool have_hbf_anchor = !hbf_points.empty();
    uint32_t hbf_anchor = have_hbf_anchor ? find_hbf_anchor(std::move(hbf_points)) : 0;

    if (have_hbf_anchor) {
        std::cout << "HBF cyclic sort anchor: " << hex24(hbf_anchor)
                  << " (" << hbf_anchor << ")\n";
        for (auto& r : records) {
            if (r.hb_seen) {
                r.hbf_sort_key = hbf_distance(r.first_hb, hbf_anchor);
            }
        }
    } else {
        std::cout << "WARNING: no HBF headers found; falling back to TimeFrame ID order.\n";
    }

    // Sort each FEE in chronological HBF order on the 24-bit ring.  We do
    // NOT use the raw numeric HBF value as the key.  Therefore a wrapped
    // 0x000000 stays after 0xffffff instead of being moved to the front.
    std::stable_sort(records.begin(), records.end(), [have_hbf_anchor](const Record& a, const Record& b) {
        if (a.femid != b.femid) {
            return a.femid < b.femid;
        }

        if (have_hbf_anchor) {
            if (a.hb_seen != b.hb_seen) {
                return a.hb_seen;
            }
            if (a.hb_seen && a.hbf_sort_key != b.hbf_sort_key) {
                return a.hbf_sort_key < b.hbf_sort_key;
            }
        }

        if (a.tfid != b.tfid) {
            return a.tfid < b.tfid;
        }
        return a.serial < b.serial;
    });

    std::map<uint32_t, HbfStats> hbf;
    for (const auto& r : records) {
        auto& s = hbf[r.femid];
        if (!r.hb_seen) {
            continue;
        }

        if (!s.seen) {
            s.seen = true;
            s.first = r.first_hb;
        } else {
            uint32_t expected = next_hbf(s.last);
            if (r.first_hb != expected) {
                ++s.discontinuities;
                if (s.first_gaps.size() < 10) {
                    s.first_gaps.emplace_back(s.last, r.first_hb);
                }
            }
        }

        s.discontinuities += r.internal_hb_gaps;
        s.count += r.hb_count;
        s.last = r.last_hb;
    }

    std::cout << "\nHBF check in sorted-output order (24-bit hbframe):\n";
    for (size_t i = 0; i < order.size(); ++i) {
        auto id = order[i];
        auto& s = hbf[id];
        std::cout << "  [" << i << "] femId=" << hex32(id) << " (" << ipv4(id) << ")"
                  << "  first=" << (s.seen ? std::to_string(s.first) : "N/A")
                  << "  last=" << (s.seen ? std::to_string(s.last) : "N/A")
                  << "  count=" << s.count
                  << "  discontinuities=" << s.discontinuities << "\n";
        for (const auto& g : s.first_gaps) {
            std::cout << "       gap: " << g.first << " -> " << g.second << "\n";
        }
    }

    bool same_start = !order.empty() && hbf[order[0]].seen;
    uint32_t start_hb = same_start ? hbf[order[0]].first : 0;
    for (auto id : order) {
        same_start &= hbf[id].seen && hbf[id].first == start_hb;
    }
    std::cout << "  start alignment: "
              << (same_start ? "OK (all equal)" : "WARNING (different)") << "\n";

    bool have_all_stops = have_hbf_anchor;
    uint32_t min_stop_pos = HBF_MASK;
    uint32_t max_stop_pos = 0;
    for (auto id : order) {
        if (!hbf[id].seen) {
            have_all_stops = false;
            break;
        }
        uint32_t pos = hbf_distance(hbf[id].last, hbf_anchor);
        min_stop_pos = std::min(min_stop_pos, pos);
        max_stop_pos = std::max(max_stop_pos, pos);
    }
    if (have_all_stops) {
        std::cout << "  stop spread    : " << (max_stop_pos - min_stop_pos)
                  << " HBF (24-bit wrap-aware)\n";
    } else {
        std::cout << "  stop spread    : N/A\n";
    }

    std::vector<std::ofstream> out(order.size());
    std::vector<fs::path> outpath(order.size());
    std::vector<uint64_t> outstf(order.size(), 0);

    for (size_t i = 0; i < order.size(); ++i) {
        std::ostringstream d;
        d << std::setw(2) << std::setfill('0') << i;
        outpath[i] = outroot / d.str() / runfile;
        fs::create_directories(outpath[i].parent_path());
        out[i].open(outpath[i], std::ios::binary | std::ios::trunc);
        if (!out[i]) {
            std::cerr << "ERROR: cannot create " << outpath[i] << "\n";
            return 12;
        }
        out[i].write(in[i].header.data(), static_cast<std::streamsize>(in[i].header.size()));
    }

    uint64_t copy_total = stf_bytes;
    uint64_t copy_done = 0;
    Progress copyprog("Copy sorted STF   ", copy_total);
    constexpr size_t BUFSZ = 4 * 1024 * 1024;
    std::vector<char> buf(BUFSZ);

    for (const auto& r : records) {
        size_t oi = outidx[r.femid];
        auto& src = in[r.input_index].f;
        src.clear();
        src.seekg(static_cast<std::streamoff>(r.offset));
        uint64_t left = r.length;

        while (left) {
            size_t n = static_cast<size_t>(std::min<uint64_t>(left, buf.size()));
            if (!read_exact(src, buf.data(), static_cast<std::streamsize>(n))) {
                std::cerr << "\nERROR: copy read failed\n";
                return 13;
            }
            out[oi].write(buf.data(), static_cast<std::streamsize>(n));
            if (!out[oi]) {
                std::cerr << "\nERROR: copy write failed\n";
                return 14;
            }
            left -= n;
            copy_done += n;
            copyprog.update(copy_done);
        }
        ++outstf[oi];
    }
    copyprog.update(copy_total, true);

    for (size_t i = 0; i < order.size(); ++i) {
        out[i].write(in[i].trailer.data(), static_cast<std::streamsize>(in[i].trailer.size()));
        out[i].close();
    }

    uint64_t output_total = 0;
    std::cout << "\nOutput (femId ascending):\n";
    for (size_t i = 0; i < order.size(); ++i) {
        auto actual = fs::file_size(outpath[i]);
        output_total += actual;
        std::cout << "  " << outpath[i]
                  << "  femId=" << hex32(order[i]) << " (" << ipv4(order[i]) << ")"
                  << "  STF=" << outstf[i] << "  bytes=" << actual << "\n";
    }

    std::cout << "\nVerification:\n"
              << "  input total  = " << input_total << " bytes\n"
              << "  output total = " << output_total << " bytes\n";
    if (input_total != output_total) {
        std::cerr << "ERROR: total size changed!\n";
        return 15;
    }

    std::cout << "  OK: total size is identical.\n"
              << "  OK: each STF was copied byte-for-byte; HBF headers/delimiters/payload are not rewritten.\n";

    bool hbf_ok = true;
    for (auto id : order) {
        hbf_ok &= hbf[id].discontinuities == 0;
    }
    std::cout << "  HBF continuity: " << (hbf_ok ? "OK" : "WARNING") << "\n";
    return hbf_ok ? 0 : 20;
}
