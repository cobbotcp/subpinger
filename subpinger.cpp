// Subnet pinger: pings every IPv4 address in a CIDR block (e.g. 1.1.1.0/31).
// Windows: links ws2_32 and iphlpapi. Run from an unprivileged prompt (ICMP allowed).

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <atomic>
#include <string>
#include <exception>
#include <thread>
#include <vector>

#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#endif

static void printUsage(const char* argv0) {
    std::fprintf(stderr,
        "Usage: %s <address>/<prefix> [options]\n"
        "  Pings the subnet in a loop until Ctrl+C. On a real console, addresses flow left to\n"
        "  right with line wrapping to the window width; colors update in place; green = reply.\n"
        "  Each address uses a dedicated thread and its own ICMP handle for that pass.\n"
        "Options:\n"
        "  --timeout-ms <n>  Per-host timeout in milliseconds (default: 1000)\n"
        "  --round-ms <n>    Extra pause in ms after each full sweep (default: 0)\n"
        "  --max <n>         Do not run if host count exceeds n (0 = no limit, default: 4096)\n"
        "  -y, --yes         Skip confirmation when above default small threshold\n"
        "  -h, --help        Show this help\n",
        argv0);
}

static bool parseU32(const char* s, uint32_t* out) {
    char* end = nullptr;
    unsigned long v = std::strtoul(s, &end, 10);
    if (!end || *end != '\0' || v > 0xFFFFFFFFu) {
        return false;
    }
    *out = static_cast<uint32_t>(v);
    return true;
}

// Parses "a.b.c.d/prefix" into network (host order) and prefix length.
static bool parseCidr(const char* cidr, uint32_t* network, int* prefix) {
    std::string s(cidr);
    const size_t slash = s.find('/');
    if (slash == std::string::npos) {
        return false;
    }
    std::string ipStr = s.substr(0, slash);
    std::string pStr = s.substr(slash + 1);
    *prefix = std::atoi(pStr.c_str());
    if (*prefix < 0 || *prefix > 32) {
        return false;
    }
    IN_ADDR addr;
    if (InetPtonA(AF_INET, ipStr.c_str(), &addr) != 1) {
        return false;
    }
    uint32_t ip = ntohl(addr.S_un.S_addr);
    if (*prefix == 0) {
        *network = 0;
    } else {
        const uint32_t mask = 0xFFFFFFFFu << (32 - *prefix);
        *network = ip & mask;
    }
    return true;
}

static uint32_t cidrHostCount(int prefix) {
    if (prefix < 0 || prefix > 32) {
        return 0;
    }
    if (prefix == 32) {
        return 1u;
    }
    return 1u << (32 - prefix);
}

static void ipv4ToString(uint32_t hostOrder, char buf[16]) {
    IN_ADDR a;
    a.S_un.S_addr = htonl(hostOrder);
    PCSTR s = inet_ntop(AF_INET, &a, buf, 16);
    if (!s) {
        std::strcpy(buf, "?.?.?.?");
    }
}

// One ICMP handle per call (or per thread in parallel); do not share one handle between threads.
static bool pingAddress(
    uint32_t host,
    const unsigned char* payload,
    unsigned payloadSize,
    std::vector<unsigned char>& replyBuf,
    DWORD timeoutMs) {
    const HANDLE hIcmp = IcmpCreateFile();
    if (hIcmp == INVALID_HANDLE_VALUE) {
        return false;
    }
    const IPAddr dst = htonl(host);
    const DWORD n = IcmpSendEcho2(
        hIcmp,
        nullptr,
        nullptr,
        nullptr,
        dst,
        const_cast<unsigned char*>(payload),
        static_cast<WORD>(payloadSize),
        nullptr,
        replyBuf.data(),
        static_cast<DWORD>(replyBuf.size()),
        timeoutMs);
    IcmpCloseHandle(hIcmp);
    if (n == 0) {
        return false;
    }
    const auto* p = reinterpret_cast<ICMP_ECHO_REPLY*>(replyBuf.data());
    return p->Status == IP_SUCCESS;
}

// Spawns one std::thread per address; each thread pings a single host, then all join. UI updates
// on the main thread after the sweep.
static void runParallelSweep(
    uint32_t network,
    uint32_t total,
    const unsigned char* payload,
    unsigned payloadSize,
    DWORD replySize,
    DWORD timeoutMs,
    std::vector<std::uint8_t>& outOnline) {
    outOnline.assign(static_cast<size_t>(total), 0u);
    if (total == 0) {
        return;
    }
    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(total));
    try {
        for (uint32_t i = 0; i < total; ++i) {
            const uint32_t index = i;
            const uint32_t host = network + i;
            workers.emplace_back(
                [index, host, &outOnline, payload, payloadSize, replySize, timeoutMs]() {
                    std::vector<unsigned char> replyBuf(static_cast<size_t>(replySize), 0u);
                    const bool ok = pingAddress(host, payload, payloadSize, replyBuf, timeoutMs);
                    outOnline[static_cast<size_t>(index)] = static_cast<std::uint8_t>(ok ? 1u : 0u);
                });
        }
    } catch (...) {
        for (auto& w : workers) {
            if (w.joinable()) {
                w.join();
            }
        }
        throw;
    }
    for (auto& w : workers) {
        w.join();
    }
}

// Console output: optional ANSI (VT) colors, else SetConsoleTextAttribute.
static HANDLE g_tuiOut = nullptr;
static bool g_tuiVt = false;
static bool g_tuiIsConsole = false;
static WORD g_tuiDefAttr = 0;
static SHORT g_dataStartY = 0;
static std::atomic_bool g_run{true};

// Left-aligned, space-padded display width so short IPs (e.g. .9) line up with longer ones (e.g. .10).
static const int TUI_IP_FIELD = 16;

static BOOL WINAPI tuiCtrlHandler(DWORD) {
    g_run.store(false, std::memory_order_relaxed);
    return TRUE;
}

static void tuiSetup() {
    g_tuiOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (g_tuiOut == INVALID_HANDLE_VALUE) {
        g_tuiOut = nullptr;
    }
    g_tuiIsConsole = false;
    g_tuiVt = false;
    if (g_tuiOut) {
        DWORD mode = 0;
        if (GetConsoleMode(g_tuiOut, &mode)) {
            g_tuiIsConsole = true;
            CONSOLE_SCREEN_BUFFER_INFO bi{};
            if (GetConsoleScreenBufferInfo(g_tuiOut, &bi)) {
                g_tuiDefAttr = bi.wAttributes;
            }
            if (SetConsoleMode(
                    g_tuiOut,
                    mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING | ENABLE_PROCESSED_OUTPUT)) {
                g_tuiVt = true;
            }
        }
    }
    SetConsoleCtrlHandler(tuiCtrlHandler, TRUE);
    std::fflush(stdout);
}

static void tuiClearScreen() {
    if (g_tuiIsConsole) {
        if (g_tuiVt) {
            std::printf("\x1b[2J\x1b[H");
        } else {
            std::system("cls");
        }
    } else {
        std::printf("\n");
    }
    std::fflush(stdout);
}

static void tuiFg(bool online) {
    if (g_tuiVt) {
        std::printf(online ? "\x1b[32;1m" : "\x1b[31;1m");
    } else if (g_tuiIsConsole && g_tuiOut) {
        const WORD a = static_cast<WORD>(
            online
                ? (FOREGROUND_GREEN | FOREGROUND_INTENSITY)
                : (FOREGROUND_RED | FOREGROUND_INTENSITY));
        SetConsoleTextAttribute(g_tuiOut, a);
    }
}

static void tuiDim() {
    if (g_tuiVt) {
        std::printf("\x1b[0m\x1b[90m");
    } else if (g_tuiIsConsole && g_tuiOut) {
        SetConsoleTextAttribute(
            g_tuiOut, static_cast<WORD>(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE));
    }
}

static void tuiReset() {
    if (g_tuiVt) {
        std::printf("\x1b[0m");
    } else if (g_tuiIsConsole && g_tuiOut) {
        SetConsoleTextAttribute(g_tuiOut, g_tuiDefAttr);
    }
}

static SHORT tuiReadCursorY() {
    if (!g_tuiOut) {
        return 0;
    }
    CONSOLE_SCREEN_BUFFER_INFO nfo{};
    if (!GetConsoleScreenBufferInfo(g_tuiOut, &nfo)) {
        return 0;
    }
    return nfo.dwCursorPosition.Y;
}

// Visible text columns (clamped). Used to wrap a horizontal flow of IPs.
static int tuiGetClientWidth() {
    if (!g_tuiIsConsole || !g_tuiOut) {
        return 80;
    }
    CONSOLE_SCREEN_BUFFER_INFO s{};
    if (!GetConsoleScreenBufferInfo(g_tuiOut, &s)) {
        return 80;
    }
    int w = s.srWindow.Right - s.srWindow.Left + 1;
    if (w < 1) {
        w = s.dwSize.X;
    }
    if (w < 8) {
        w = 8;
    }
    return w;
}

// Pack host indices into rows using fixed cell width (TUI_IP_FIELD) so wrap matches aligned columns.
static std::vector<std::vector<uint32_t>> tuiBuildWrapRows(const uint32_t total, int width) {
    std::vector<std::vector<uint32_t>> rows;
    if (total == 0) {
        return rows;
    }
    if (width < 4) {
        width = 40;
    }
    const int cellW = TUI_IP_FIELD; // per-cell display width (line layout: "  " + N*(gap+field))
    std::vector<uint32_t> cur;
    int endCol = 0; // 0 = line empty; else exclusive end column
    for (uint32_t i = 0; i < total; ++i) {
        if (endCol == 0) {
            cur.push_back(i);
            endCol = 2 + cellW; // "  " + field
            continue;
        }
        if (endCol + 2 + cellW > width) {
            rows.push_back(std::move(cur));
            cur.clear();
            endCol = 0;
            --i;
            continue;
        }
        cur.push_back(i);
        endCol += 2 + cellW;
    }
    if (!cur.empty()) {
        rows.push_back(std::move(cur));
    }
    return rows;
}

// Grow screen buffer if the wrapped IP list needs more lines than the buffer.
static void tuiEnsureListFitsInBuffer(const uint32_t lineCount) {
    if (!g_tuiIsConsole || !g_tuiOut) {
        return;
    }
    CONSOLE_SCREEN_BUFFER_INFO s{};
    if (!GetConsoleScreenBufferInfo(g_tuiOut, &s)) {
        return;
    }
    const int needY = static_cast<int>(g_dataStartY) + static_cast<int>(lineCount) + 4;
    if (needY > static_cast<int>(s.dwSize.Y) && needY > 0) {
        COORD newSize{};
        newSize.X = s.dwSize.X;
        if (needY > 9999) {
            newSize.Y = 9999;
        } else {
            newSize.Y = static_cast<SHORT>(needY);
        }
        SetConsoleScreenBufferSize(g_tuiOut, newSize);
    }
}

static void tuiGoto(const SHORT x, const SHORT y) {
    if (!g_tuiOut) {
        return;
    }
    SetConsoleCursorPosition(g_tuiOut, {x, y});
}

// One wrapped row: "  " margin, then 2-space gap + left-padded 16-col IP field for each address (line cleared first).
static void tuiRenderWrappedConsoleLine(
    const SHORT lineY,
    const std::vector<uint32_t>& hostIndices,
    const uint32_t network,
    const std::vector<std::uint8_t>& onl,
    const int clearWidth) {
    tuiGoto(0, lineY);
    tuiReset();
    if (g_tuiVt) {
        std::printf("\x1b[2K");
    } else {
        for (int k = 0; k < clearWidth; ++k) {
            std::putchar(' ');
        }
        tuiGoto(0, lineY);
    }
    if (hostIndices.empty()) {
        std::fflush(stdout);
        return;
    }
    SHORT col = 0;
    for (size_t j = 0; j < hostIndices.size(); ++j) {
        const uint32_t idx = hostIndices[j];
        char s[16];
        ipv4ToString(network + idx, s);
        if (j == 0) {
            tuiGoto(0, lineY);
            tuiReset();
            std::printf("  ");
            col = 2;
        } else {
            tuiGoto(col, lineY);
            tuiReset();
            std::printf("  ");
            col = static_cast<SHORT>(col + 2);
        }
        tuiGoto(col, lineY);
        const bool on = onl[static_cast<size_t>(idx)] != 0u;
        tuiFg(on);
        std::printf("%-*s", TUI_IP_FIELD, s);
        tuiReset();
        col = static_cast<SHORT>(col + static_cast<SHORT>(TUI_IP_FIELD));
    }
    if (g_tuiVt) {
        tuiGoto(col, lineY);
        std::printf("\x1b[K");
    }
    std::fflush(stdout);
}

// Redirect: same wrapping, newlines between rows, no cursor control.
static void tuiRenderWrappedNonConsole(
    const std::vector<std::vector<uint32_t>>& rows,
    const uint32_t network,
    const std::vector<std::uint8_t>& onl) {
    for (size_t r = 0; r < rows.size(); ++r) {
        const std::vector<uint32_t>& hostIndices = rows[r];
        for (size_t j = 0; j < hostIndices.size(); ++j) {
            const uint32_t idx = hostIndices[j];
            char s[16];
            ipv4ToString(network + idx, s);
            tuiReset();
            std::printf("  ");
            const bool on = onl[static_cast<size_t>(idx)] != 0u;
            tuiFg(on);
            std::printf("%-*s", TUI_IP_FIELD, s);
            tuiReset();
        }
        std::printf("\n");
        std::fflush(stdout);
    }
}

static void tuiPrintBanner(const char* cidr, uint32_t total, DWORD timeoutMs, DWORD roundMs) {
    tuiDim();
    if (g_tuiVt) {
        std::printf(
            "  \x1b[1m%s\x1b[0m\x1b[90m  |  %u host%s  |  %lu ms timeout  |  %lu ms between sweeps  |  Ctrl+C to stop  | github.com/cobbotcp/subpinger\x1b[0m\n\n",
            cidr,
            static_cast<unsigned>(total),
            total == 1 ? "" : "s",
            static_cast<unsigned long>(timeoutMs),
            static_cast<unsigned long>(roundMs));
    } else {
        tuiReset();
        std::printf("  %s  |  %u host%s  |  %lu ms timeout  |  %lu ms between sweeps  |  Ctrl+C to stop\n\n",
            cidr,
            static_cast<unsigned>(total),
            total == 1 ? "" : "s",
            static_cast<unsigned long>(timeoutMs),
            static_cast<unsigned long>(roundMs));
    }
    tuiReset();
    std::fflush(stdout);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }
    if (std::strcmp(argv[1], "-h") == 0 || std::strcmp(argv[1], "--help") == 0) {
        printUsage(argv[0]);
        return 0;
    }

    const char* cidr = argv[1];
    uint32_t network = 0;
    int prefix = 0;
    if (!parseCidr(cidr, &network, &prefix)) {
        std::fprintf(stderr, "Error: bad CIDR (expected e.g. 1.1.1.0/31).\n");
        return 1;
    }

    DWORD timeoutMs = 1000;
    DWORD roundDelayMs = 0;
    uint32_t maxHosts = 4096;
    bool skipConfirm = false;

    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "-y") == 0 || std::strcmp(argv[i], "--yes") == 0) {
            skipConfirm = true;
        } else if (std::strcmp(argv[i], "--timeout-ms") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "Error: --timeout-ms needs a value.\n");
                return 1;
            }
            unsigned long t = std::strtoul(argv[++i], nullptr, 10);
            if (t < 1 || t > 600000) {
                std::fprintf(stderr, "Error: timeout out of range (1..600000 ms).\n");
                return 1;
            }
            timeoutMs = static_cast<DWORD>(t);
        } else if (std::strcmp(argv[i], "--round-ms") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "Error: --round-ms needs a value.\n");
                return 1;
            }
            const unsigned long t = std::strtoul(argv[++i], nullptr, 10);
            if (t > 3ul * 24 * 60 * 60 * 1000) {
                std::fprintf(stderr, "Error: --round-ms out of range.\n");
                return 1;
            }
            roundDelayMs = static_cast<DWORD>(t);
        } else if (std::strcmp(argv[i], "--max") == 0) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "Error: --max needs a value.\n");
                return 1;
            }
            if (!parseU32(argv[++i], &maxHosts)) {
                std::fprintf(stderr, "Error: bad --max value.\n");
                return 1;
            }
        } else {
            std::fprintf(stderr, "Error: unknown option %s\n", argv[i]);
            printUsage(argv[0]);
            return 1;
        }
    }

    const uint32_t total = cidrHostCount(prefix);
    if (maxHosts != 0 && total > maxHosts) {
        std::fprintf(stderr,
            "Error: %s has %u addresses; max allowed is %u (use --max 0 to disable).\n",
            cidr, static_cast<unsigned>(total), static_cast<unsigned>(maxHosts));
        return 1;
    }

    if (total > 256 && !skipConfirm) {
        std::fprintf(stderr,
            "Warning: about to ping %u addresses. Re-run with -y to confirm.\n",
            static_cast<unsigned>(total));
        return 1;
    }

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::fprintf(stderr, "Error: WSAStartup failed.\n");
        return 1;
    }

    {
        const HANDLE hProbe = IcmpCreateFile();
        if (hProbe == INVALID_HANDLE_VALUE) {
            std::fprintf(stderr, "Error: IcmpCreateFile failed (are you on Windows?).\n");
            WSACleanup();
            return 1;
        }
        IcmpCloseHandle(hProbe);
    }

    const unsigned char payload[32] = {0};
    const unsigned payloadSize = static_cast<unsigned>(sizeof(payload));
    const DWORD replySize = sizeof(ICMP_ECHO_REPLY) + payloadSize + 128;
    std::vector<std::uint8_t> outOnline;
    outOnline.reserve(static_cast<size_t>(total));

    tuiSetup();

    if (g_tuiIsConsole) {
        tuiClearScreen();
    }
    tuiPrintBanner(cidr, total, timeoutMs, roundDelayMs);
    if (g_tuiIsConsole) {
        g_dataStartY = tuiReadCursorY();
        {
            const int wInit = tuiGetClientWidth();
            const std::vector<std::vector<uint32_t>> initRows = tuiBuildWrapRows(total, wInit);
            tuiEnsureListFitsInBuffer(static_cast<uint32_t>(initRows.size()));
        }
        if (g_tuiVt) {
            std::printf("\x1b[?25l");
            std::fflush(stdout);
        }
    }

    int sweepIndex = 0;
    while (g_run.load(std::memory_order_relaxed)) {
        if (!g_tuiIsConsole) {
            if (sweepIndex > 0) {
                tuiReset();
                std::printf("\n---\n");
            }
        }
        ++sweepIndex;

        if (!g_run.load(std::memory_order_relaxed)) {
            break;
        }
        try {
            runParallelSweep(
                network, total, payload, payloadSize, replySize, timeoutMs, outOnline);
        } catch (const std::exception& e) {
            tuiReset();
            std::fprintf(
                stderr,
                "Error: could not start ping threads (try a smaller subnet or use --max).\n  %s\n",
                e.what());
            WSACleanup();
            return 1;
        }

        {
            const int wrapW = tuiGetClientWidth();
            const std::vector<std::vector<uint32_t>> layoutRows = tuiBuildWrapRows(total, wrapW);
            tuiEnsureListFitsInBuffer(static_cast<uint32_t>(layoutRows.size()));
            if (g_tuiIsConsole) {
                const int clearW = tuiGetClientWidth();
                for (size_t r = 0; r < layoutRows.size(); ++r) {
                    tuiRenderWrappedConsoleLine(
                        static_cast<SHORT>(g_dataStartY + static_cast<int>(r)),
                        layoutRows[r],
                        network,
                        outOnline,
                        clearW);
                }
            } else {
                tuiRenderWrappedNonConsole(layoutRows, network, outOnline);
            }
        }

        if (!g_run.load(std::memory_order_relaxed)) {
            break;
        }
        if (roundDelayMs) {
            DWORD rem = roundDelayMs;
            while (rem > 0 && g_run.load(std::memory_order_relaxed)) {
                const DWORD step = rem > 100 ? 100 : rem;
                Sleep(step);
                rem -= step;
            }
        } else {
            Sleep(0);
        }
    }

    WSACleanup();

    tuiReset();
    if (g_tuiIsConsole && g_tuiOut) {
        if (g_tuiVt) {
            std::printf("\x1b[?25h");
        }
        {
            const int wEnd = tuiGetClientWidth();
            const std::vector<std::vector<uint32_t>> endRows = tuiBuildWrapRows(total, wEnd);
            const COORD endPos{0, static_cast<SHORT>(g_dataStartY + static_cast<int>(endRows.size()))};
            SetConsoleCursorPosition(g_tuiOut, endPos);
        }
    }
    if (g_tuiIsConsole) {
        std::printf("Stopped.\n");
    } else {
        std::printf("Stopped (redirected output).\n");
    }
    return 0;
}
