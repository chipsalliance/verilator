// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: File stream wrapper that understands indentation
//
// Code available from: https://verilator.org
//
//*************************************************************************
//
// Copyright 2003-2025 by Wilson Snyder. This program is free software; you
// can redistribute it and/or modify it under the terms of either the GNU
// Lesser General Public License Version 3 or the Perl Artistic License
// Version 2.0.
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//*************************************************************************

#include "V3PchAstMT.h"

#include "V3File.h"

#include "V3Os.h"
#include "V3String.h"

#include <cerrno>
#include <cstdarg>
#include <fcntl.h>
#include <iomanip>
#include <map>
#include <memory>

#include <sys/stat.h>
#include <sys/types.h>

// clang-format off
#if defined(__unix__) || defined(__unix) || (defined(__APPLE__) && defined(__MACH__))
# define INFILTER_PIPE  // Allow pipe filtering.  Needs fork()
#endif

#ifdef HAVE_STAT_NSEC  // i.e. Linux 2.6, from configure
# define VL_STAT_CTIME_NSEC(stat) ((stat).st_ctim.tv_nsec)  // Nanoseconds
# define VL_STAT_MTIME_NSEC(stat) ((stat).st_mtim.tv_nsec)  // Nanoseconds
#else
# define VL_STAT_CTIME_NSEC(stat) (0)
# define VL_STAT_MTIME_NSEC(stat) (0)
#endif

#ifdef INFILTER_PIPE
# include <sys/wait.h>
#endif

#if defined(_WIN32) || defined(__MINGW32__)
# include <io.h>  // open, read, write, close
#endif
// clang-format on

VL_DEFINE_DEBUG_FUNCTIONS;

// If change this code, run a test with the below size set very small
// #define INFILTER_IPC_BUFSIZ 16
constexpr int INFILTER_IPC_BUFSIZ = (64 * 1024);  // For debug, try this as a small number
constexpr int INFILTER_CACHE_MAX = (64 * 1024);  // Maximum bytes to cache if same file read twice

constexpr off_t FILE_HASH_SIZE_MAX = 1 * 1024 * 1024;  // Maxium size file to hash

//######################################################################
// V3File Internal state

class V3FileDependImp final {
    // TYPES
    class DependFile final {
        // A single file
        const bool m_target;  // True if write, else read
        bool m_exists = true;
        const string m_filename;  // Filename
        struct stat m_stat;  // Stat information
        VHashSha256 m_hash;  // SHA hash of file contents
    public:
        DependFile(const string& filename, bool target)
            : m_target{target}
            , m_filename{filename} {
            m_stat.st_ctime = 0;
            m_stat.st_mtime = 0;
        }
        ~DependFile() = default;
        const string& filename() const { return m_filename; }
        bool target() const { return m_target; }
        bool exists() const { return m_exists; }
        off_t size() const { return m_stat.st_size; }
        ino_t ino() const { return m_stat.st_ino; }
        time_t cstime() const { return m_stat.st_ctime; }  // Seconds
        time_t cnstime() const { return VL_STAT_CTIME_NSEC(m_stat); }  // Nanoseconds
        time_t mstime() const { return m_stat.st_mtime; }  // Seconds
        time_t mnstime() const { return VL_STAT_MTIME_NSEC(m_stat); }  // Nanoseconds
        string hashDigestSymbol() {
            static VHashSha256 emptyHash;
            return m_hash.digestSymbol() != emptyHash.digestSymbol() ? m_hash.digestSymbol()
                                                                     : "unhashed";
        }
        void loadStats() {
            if (!m_stat.st_mtime) {
                const string fn = filename();
                const int err = stat(fn.c_str(), &m_stat);
                if (err != 0) {
                    std::memset(&m_stat, 0, sizeof(m_stat));
                    m_stat.st_mtime = 1;
                    m_exists = false;
                    // Not an error... This can occur due to `line directives in the .vpp files
                    UINFO(1, "-Info: File not statable: " << filename());
                } else {
                    // For speed, don't hash large files (e.g. verilator_bin)
                    if (!target() && size() <= FILE_HASH_SIZE_MAX) m_hash.insertFile(filename());
                }
            }
        }
        bool operator<(const DependFile& rhs) const { return filename() < rhs.filename(); }
    };

    // MEMBERS
    V3Mutex m_mutex;  // Protects members
    std::set<string> m_filenameSet VL_GUARDED_BY(m_mutex);  // Files generated (elim duplicates)
    std::set<DependFile> m_filenameList;  // Files sourced/generated

    static string stripQuotes(const string& in) {
        string pretty = in;
        string::size_type pos;
        while ((pos = pretty.find('\"')) != string::npos) pretty.replace(pos, 1, "_");
        while ((pos = pretty.find('\n')) != string::npos) pretty.replace(pos, 1, "_");
        return pretty;
    }

public:
    // ACCESSOR METHODS
    void addSrcDepend(const string& filename) VL_MT_SAFE_EXCLUDES(m_mutex) {
        const V3LockGuard lock{m_mutex};
        const auto itFoundPair = m_filenameSet.insert(filename);
        if (itFoundPair.second) {
            DependFile df{filename, false};
            df.loadStats();  // Get size now, in case changes during the run
            m_filenameList.insert(df);
        }
    }
    void addTgtDepend(const string& filename) VL_MT_SAFE_EXCLUDES(m_mutex) {
        const V3LockGuard lock{m_mutex};
        const auto itFoundPair = m_filenameSet.insert(filename);
        if (itFoundPair.second) m_filenameList.insert(DependFile{filename, true});
    }
    void writeDepend(const string& filename);
    std::vector<string> getAllDeps() const;
    void writeTimes(const string& filename, const string& cmdlineIn);
    bool checkTimes(const string& filename, const string& cmdlineIn);
};

V3FileDependImp dependImp;  // Depend implementation class

//######################################################################
// V3FileDependImp

void V3FileDependImp::writeDepend(const string& filename) {
    const std::unique_ptr<std::ofstream> ofp{V3File::new_ofstream(filename)};
    if (ofp->fail()) v3fatal("Can't write file: " << filename);

    for (const DependFile& i : m_filenameList) {
        if (i.target()) *ofp << i.filename() << " ";
    }
    *ofp << " : ";
    *ofp << v3Global.opt.buildDepBin();
    *ofp << " ";

    for (const DependFile& i : m_filenameList) {
        if (!i.target()) *ofp << i.filename() << " ";
    }

    *ofp << '\n';

    if (v3Global.opt.makePhony()) {
        *ofp << '\n';
        for (const DependFile& i : m_filenameList) {
            if (!i.target()) *ofp << i.filename() << ":\n";
        }
    }
}

std::vector<string> V3FileDependImp::getAllDeps() const {
    std::vector<string> r;
    for (const auto& itr : m_filenameList) {
        if (!itr.target() && itr.exists()) r.push_back(itr.filename());
    }
    return r;
}

void V3FileDependImp::writeTimes(const string& filename, const string& cmdlineIn) {
    const std::unique_ptr<std::ofstream> ofp{V3File::new_ofstream(filename)};
    if (ofp->fail()) v3fatal("Can't write file: " << filename);

    const string cmdline = stripQuotes(cmdlineIn);
    *ofp << "# DESCR"
         << "IPTION: Verilator output: Timestamp data for --skip-identical.  Delete at will.\n";
    *ofp << "C \"" << cmdline << "\"\n";

    for (auto& it : m_filenameList) {
        DependFile* const depfilep = const_cast<DependFile*>(&it);
        // Read stats of files we create after we're done making them
        // (except for this file, of course)
        V3Os::filesystemFlush(depfilep->filename());
        depfilep->loadStats();
        off_t showSize = depfilep->size();
        ino_t showIno = depfilep->ino();
        if (depfilep->filename() == filename) {
            showSize = 0;
            showIno = 0;  // We're writing it, so need to ignore it
        }

        *ofp << (depfilep->target() ? "T" : "S") << " ";
        *ofp << " " << std::setw(8) << showSize;
        *ofp << " " << std::setw(8) << showIno;
        *ofp << " " << std::setw(11) << depfilep->cstime();
        *ofp << " " << std::setw(11) << depfilep->cnstime();
        *ofp << " " << std::setw(11) << depfilep->mstime();
        *ofp << " " << std::setw(11) << depfilep->mnstime();
        *ofp << " \"" << depfilep->hashDigestSymbol() << "\"";
        *ofp << " \"" << depfilep->filename() << "\"";
        *ofp << '\n';
    }
}

bool V3FileDependImp::checkTimes(const string& filename, const string& cmdlineIn) {
    const std::unique_ptr<std::ifstream> ifp{V3File::new_ifstream_nodepend(filename)};
    if (ifp->fail()) {
        UINFO(2, "   --check-times failed: no input " << filename);
        return false;
    }
    {
        const string ignore = V3Os::getline(*ifp);
        if (ignore.empty()) { /*used*/
        }
    }
    {
        char chkDir;
        *ifp >> chkDir;
        char quote;
        *ifp >> quote;
        const string chkCmdline = V3Os::getline(*ifp, '"');
        const string cmdline = stripQuotes(cmdlineIn);
        if (cmdline != chkCmdline) {
            UINFO(2, "   --check-times failed: different command line");
            return false;
        }
    }

    const bool skipHashing = !V3Os::getenvStr("VERILATOR_DEBUG_SKIP_HASH", "").empty();

    while (!ifp->eof()) {
        char chkDir;
        *ifp >> chkDir;
        off_t chkSize;
        *ifp >> chkSize;
        ino_t chkIno;
        *ifp >> chkIno;
        if (ifp->eof()) break;  // Needed to read final whitespace before found eof
        time_t chkCstime;
        *ifp >> chkCstime;
        time_t chkCnstime;
        *ifp >> chkCnstime;
        time_t chkMstime;
        *ifp >> chkMstime;
        time_t chkMnstime;
        *ifp >> chkMnstime;
        char quote;
        *ifp >> quote;
        const string chkHash = V3Os::getline(*ifp, '"');
        *ifp >> quote;
        const string chkFilename = V3Os::getline(*ifp, '"');

        V3Os::filesystemFlush(chkFilename);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
        struct stat curStat;
        const int err = stat(chkFilename.c_str(), &curStat);
        if (err != 0) {
            UINFO(2, "   --check-times failed: missing " << chkFilename);
            return false;
        }

        // Skip the .dat file itself, as we were writing it at the time was being made
        if (filename == chkFilename) continue;

        UINFO(9, " got d=" << chkDir << " s=" << chkSize << " ct=" << chkCstime << "."
                           << chkCnstime << " mt=" << chkMstime << "." << chkMnstime
                           << " h=" << chkHash << " fn= " << chkFilename);
        UINFO(9, " nowSt  s=" << curStat.st_size << " mt=" << curStat.st_mtime
                              << " ct=" << curStat.st_ctime);

        // We'd like this rule:
        // if (!(curStat.st_size == chkSize
        //      && curStat.st_mtime == chkMstime) {
        // However NFS messes us up, as there might be some data outstanding when
        // we determined the original size.  For safety, we know the creation time
        // must be within a few second window... call it 20 sec.
        if (!(curStat.st_size >= chkSize && curStat.st_ino == chkIno
              && curStat.st_ctime == chkCstime && VL_STAT_CTIME_NSEC(curStat) == chkCnstime
              && curStat.st_mtime <= (chkMstime + 20)
              // Not comparing chkMnstime
              )) {
            UINFO(2, "   --check-times: out-of-date "
                         << chkFilename << "; " << curStat.st_size << "=?" << chkSize << " "
                         << curStat.st_ctime << "." << VL_STAT_CTIME_NSEC(curStat) << "=?"
                         << chkCstime << "." << chkCnstime << " " << curStat.st_mtime << "."
                         << VL_STAT_MTIME_NSEC(curStat) << "=?" << chkMstime << "." << chkMnstime
                         << endl);
            if (skipHashing) return false;
            // We didn't hash target output files, nor large files,
            // as unlikely to find a match and can be large
            if (chkHash == "unhashed") return false;

            VHashSha256 curHash;
            curHash.insertFile(chkFilename);
            if (curHash.digestSymbol() != chkHash) {
                UINFO(2, "   --check-times: hash differs "
                             << chkFilename << "; " << curHash.digestSymbol() << "=?" << chkHash);
                return false;
            }
        }
    }
    return true;
}

//######################################################################
// V3File

void V3File::addSrcDepend(const string& filename) VL_MT_SAFE { dependImp.addSrcDepend(filename); }
void V3File::addTgtDepend(const string& filename) VL_MT_SAFE { dependImp.addTgtDepend(filename); }
void V3File::writeDepend(const string& filename) { dependImp.writeDepend(filename); }
std::vector<string> V3File::getAllDeps() { return dependImp.getAllDeps(); }
void V3File::writeTimes(const string& filename, const string& cmdlineIn) {
    dependImp.writeTimes(filename, cmdlineIn);
}
bool V3File::checkTimes(const string& filename, const string& cmdlineIn) {
    return dependImp.checkTimes(filename, cmdlineIn);
}
void V3File::createMakeDirFor(const string& filename) {
    if (filename != VL_DEV_NULL
        // If doesn't start with makeDir then some output file user requested
        && filename.substr(0, v3Global.opt.makeDir().length() + 1)
               == v3Global.opt.makeDir() + "/") {
        createMakeDir();
    }
}
void V3File::createMakeDir() {
    static bool created = false;
    if (!created) {
        created = true;
        V3Os::createDir(v3Global.opt.makeDir());
        if (v3Global.opt.hierTop()) V3Os::createDir(v3Global.opt.hierTopDataDir());
    }
}

//######################################################################
// VInFilterImp

class VInFilterImp final {
    using StrList = VInFilter::StrList;

    std::map<const std::string, std::string> m_contentsMap;  // Cache of file contents
    bool m_readEof = false;  // Received EOF on read
#ifdef INFILTER_PIPE
    pid_t m_pid = 0;  // fork() process id
#else
    int m_pid = 0;  // fork() process id - always zero as disabled
#endif
    bool m_pidExited = false;
    int m_pidStatus = 0;
    int m_writeFd = 0;  // File descriptor TO filter
    int m_readFd = 0;  // File descriptor FROM filter

private:
    // METHODS

    bool readContents(const string& filename, StrList& outl) {
        if (m_pid) {
            return readContentsFilter(filename, outl);
        } else {
            return readContentsFile(filename, outl);
        }
    }
    bool readContentsFile(const string& filename, StrList& outl) {
        const int fd = open(filename.c_str(), O_RDONLY);
        if (fd < 0) return false;
        m_readEof = false;
        readBlocks(fd, -1, outl);
        close(fd);
        return true;
    }
    bool readContentsFilter(const string& filename, StrList& outl) {
        (void)filename;  // Prevent unused variable warning
        (void)outl;  // Prevent unused variable warning
#ifdef INFILTER_PIPE
        writeFilter("read \"" + filename + "\"\n");
        const string line = readFilterLine();
        if (line.find("Content-Length") != string::npos) {
            int len = 0;
            sscanf(line.c_str(), "Content-Length: %d\n", &len);
            readBlocks(m_readFd, len, outl);
            return true;
        } else {
            if (line != "") v3error("--pipe-filter protocol error, unexpected: " << line);
            return false;
        }
#else
        v3fatalSrc("--pipe-filter not implemented on this platform");
        return false;
#endif
    }

    // cppcheck-suppress functionConst
    void checkFilter(bool hang) {
#ifdef INFILTER_PIPE
        if (!m_pidExited && waitpid(m_pid, &m_pidStatus, hang ? 0 : WNOHANG)) {
            UINFO(1, "--pipe-filter: Exited, status "
                         << m_pidStatus << " exit=" << WEXITSTATUS(m_pidStatus) << " err"
                         << std::strerror(errno));
            m_readEof = true;
            m_pidExited = true;
        }
#endif
    }

    void readBlocks(int fd, int size, StrList& outl) {
        char buf[INFILTER_IPC_BUFSIZ];
        ssize_t sizegot = 0;
        while (!m_readEof && (size < 0 || size > sizegot)) {
            ssize_t todo = INFILTER_IPC_BUFSIZ;
            if (size > 0 && size < todo) todo = size;
            errno = 0;
            const ssize_t got = read(fd, buf, todo);
            // UINFO(9, "RD GOT g " << got << " e " << errno << " " << strerror(errno));
            // usleep(50*1000);
            if (got > 0) {
                outl.push_back(string(buf, got));
                sizegot += got;
            } else if (errno == EINTR || errno == EAGAIN
#ifdef EWOULDBLOCK
                       || errno == EWOULDBLOCK
#endif
            ) {
                checkFilter(false);
                V3Os::u_sleep(1000);
                continue;
            } else {
                m_readEof = true;
                break;
            }
        }
    }
    // cppverilator-suppress unusedFunction unusedPrivateFunction
    string readFilterLine() {
        // Slow, but we don't need it much
        UINFO(9, "readFilterLine");
        string line;
        while (!m_readEof) {
            StrList outl;
            readBlocks(m_readFd, 1, outl);
            const string onechar = listString(outl);
            line += onechar;
            if (onechar == "\n") {
                if (line == "\n") {
                    line = "";
                    continue;
                } else {
                    break;
                }
            }
        }
        UINFO(6, "filter-line-in: " << line);
        return line;
    }
    // cppverilator-suppress unusedFunction unusedPrivateFunction
    void writeFilter(const string& out) {
        if (debug() >= 6) {
            UINFO(6, "filter-out: " << out);
            if (out[out.length() - 1] != '\n') cout << endl;
        }
        if (!m_pid) {
            v3error("--pipe-filter: write to closed file\n");
            m_readEof = true;
            stop();
        }
        unsigned offset = 0;
        while (!m_readEof && out.length() > offset) {
            errno = 0;
            const int got = write(m_writeFd, (out.c_str()) + offset, out.length() - offset);
            // UINFO(9, "WR GOT g " << got << " e " << errno << " " << strerror(errno));
            // usleep(50*1000);
            if (got > 0) {
                offset += got;
            } else if (errno == EINTR || errno == EAGAIN
#ifdef EWOULDBLOCK
                       || errno == EWOULDBLOCK
#endif
            ) {
                checkFilter(false);
                V3Os::u_sleep(1000);
                continue;
            } else {
                break;
            }
        }
    }

    // Start the filter
    void start(const string& command) {
        if (command == "") {
            m_pid = 0;  // Disabled
        } else {
            startFilter(command);
        }
    }
    void startFilter(const string& command) {
        (void)command;  // Prevent Unused variable warning
#ifdef INFILTER_PIPE
        int fd_stdin[2];  // Can't use std::array
        int fd_stdout[2];  // Can't use std::array
        constexpr int P_RD = 0;
        constexpr int P_WR = 1;

        if (pipe(fd_stdin) != 0 || pipe(fd_stdout) != 0) {
            v3fatal("--pipe-filter: Can't pipe: " << std::strerror(errno));
        }
        if (fd_stdin[P_RD] <= 2 || fd_stdin[P_WR] <= 2 || fd_stdout[P_RD] <= 2
            || fd_stdout[P_WR] <= 2) {
            // We'd have to rearrange all of the FD usages in this case.
            // Too unlikely; verilator isn't a daemon.
            v3fatal("--pipe-filter: stdin/stdout closed before pipe opened\n");
        }

        UINFO(1, "--pipe-filter: /bin/sh -c " << command);

        const pid_t pid = fork();
        if (pid < 0) v3fatal("--pipe-filter: fork failed: " << std::strerror(errno));
        if (pid == 0) {  // Child
            UINFO(6, "In child");
            close(fd_stdin[P_WR]);
            dup2(fd_stdin[P_RD], 0);
            close(fd_stdout[P_RD]);
            dup2(fd_stdout[P_WR], 1);
            // And stderr comes from parent

            execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
            // Don't use v3fatal, we don't share the common structures any more
            fprintf(stderr, "--pipe-filter: exec failed: %s\n", std::strerror(errno));
            _exit(1);
        } else {  // Parent
            UINFO(6, "In parent, child pid " << pid << " stdin " << fd_stdin[P_WR] << "->"
                                             << fd_stdin[P_RD] << " stdout " << fd_stdout[P_WR]
                                             << "->" << fd_stdout[P_RD]);
            m_pid = pid;
            m_pidExited = false;
            m_pidStatus = 0;
            m_writeFd = fd_stdin[P_WR];
            m_readFd = fd_stdout[P_RD];
            m_readEof = false;

            close(fd_stdin[P_RD]);
            close(fd_stdout[P_WR]);

            int flags = fcntl(m_readFd, F_GETFL, 0);
            fcntl(m_readFd, F_SETFL, flags | O_NONBLOCK);

            flags = fcntl(m_writeFd, F_GETFL, 0);
            fcntl(m_writeFd, F_SETFL, flags | O_NONBLOCK);
        }
        UINFO(6, "startFilter complete");
#else
        v3fatalSrc("--pipe-filter not implemented on this platform");
#endif
    }

    void stop() {
        if (m_pid) stopFilter();
    }
    void stopFilter() {
        UINFO(6, "Stopping filter process");
#ifdef INFILTER_PIPE
        close(m_writeFd);
        checkFilter(true);
        if (!WIFEXITED(m_pidStatus) || WEXITSTATUS(m_pidStatus) != 0) {
            v3fatal("--pipe-filter returned bad status");
        }
        m_pid = 0;
        close(m_readFd);
        UINFO(6, "Closed");
#else
        v3fatalSrc("--pipe-filter not implemented on this platform");
#endif
    }

protected:
    friend class VInFilter;
    // Read file contents and return it
    bool readWholefile(const string& filename, StrList& outl) {
        const auto it = m_contentsMap.find(filename);
        if (it != m_contentsMap.end()) {
            outl.push_back(it->second);
            return true;
        }
        if (!readContents(filename, outl)) return false;
        if (listSize(outl) < INFILTER_CACHE_MAX) {
            // Cache small files (only to save space)
            // It's quite common to `include "timescale" thousands of times
            // This isn't so important if it's just an open(), but filtering can be slow
            m_contentsMap.emplace(filename, listString(outl));
        }
        return true;
    }
    static size_t listSize(const StrList& sl) {
        size_t result = 0;
        for (const string& i : sl) result += i.length();
        return result;
    }
    static string listString(const StrList& sl) {
        string result;
        for (const string& i : sl) result += i;
        return result;
    }
    // CONSTRUCTORS
    explicit VInFilterImp(const string& command) { start(command); }
    ~VInFilterImp() { stop(); }
};

//######################################################################
// VInFilter
// Just dispatch to the implementation

VInFilter::VInFilter(const string& command) { m_impp = new VInFilterImp{command}; }
VInFilter::~VInFilter() {
    if (m_impp) VL_DO_CLEAR(delete m_impp, m_impp = nullptr);
}

bool VInFilter::readWholefile(const string& filename, VInFilter::StrList& outl) {
    UASSERT(m_impp, "readWholefile on invalid filter");
    return m_impp->readWholefile(filename, outl);
}

//######################################################################
// V3OutFormatter: A class for printing code with automatic indentation.

V3OutFormatter::V3OutFormatter(V3OutFormatter::Language lang)
    : m_lang{lang} {
    m_blockIndent = v3Global.opt.decoration() ? 4 : 1;
    m_commaWidth = v3Global.opt.decoration() ? 50 : 150;
}

//----------------------------------------------------------------------

string V3OutFormatter::indentSpaces(int num) {
    // Indent the specified number of spaces.
    if (num <= 0) return std::string{};
    return std::string(std::min<size_t>(num, MAXSPACE), ' ');
}

bool V3OutFormatter::tokenMatch(const char* cp, const char* cmp) {
    while (*cmp && *cmp == *cp) {
        ++cp;
        ++cmp;
    }
    if (*cmp) return false;
    if (*cp && !std::isspace(*cp)) return false;
    return true;
}

bool V3OutFormatter::tokenStart(const char* cp) {
    return (tokenMatch(cp, "begin") || tokenMatch(cp, "case") || tokenMatch(cp, "casex")
            || tokenMatch(cp, "casez") || tokenMatch(cp, "class") || tokenMatch(cp, "function")
            || tokenMatch(cp, "interface") || tokenMatch(cp, "module") || tokenMatch(cp, "package")
            || tokenMatch(cp, "task"));
}

bool V3OutFormatter::tokenEnd(const char* cp) {
    return (tokenMatch(cp, "end") || tokenMatch(cp, "endcase") || tokenMatch(cp, "endclass")
            || tokenMatch(cp, "endfunction") || tokenMatch(cp, "endinterface")
            || tokenMatch(cp, "endmodule") || tokenMatch(cp, "endpackage")
            || tokenMatch(cp, "endtask"));
}

bool V3OutFormatter::tokenNotStart(const char* cp) {
    return (tokenMatch(cp, "export") || tokenMatch(cp, "import"));
}

int V3OutFormatter::endLevels(const char* strg) {
    int levels = m_indentLevel;
    {
        const char* cp = strg;
        while (std::isspace(*cp)) ++cp;
        switch (*cp) {
        case '\n':  // Newlines.. No need for whitespace before it
            return 0;
        case '#':  // Preproc directive
            return 0;
        }
        {
            // label/public/private:  Deindent by 2 spaces
            const char* mp = cp;
            for (; std::isalnum(*mp); ++mp) {}
            if (mp[0] == ':' && mp[1] != ':') return (levels - m_blockIndent / 2);
        }
    }
    // We want "} else {" to be one level to the left of normal
    for (const char* cp = strg; *cp; ++cp) {
        switch (*cp) {
        case '}':
        case ')': levels -= m_blockIndent; break;
        case '<':
            if (m_lang == LA_XML) {
                if (cp[1] == '/') levels -= m_blockIndent;
            }
            break;
        case 'e':
            if (m_lang == LA_VERILOG && tokenEnd(cp)) levels -= m_blockIndent;
            break;
        case '\t':
        case ' ': break;  // Continue
        default: return levels;  // Letter
        }
    }
    return levels;
}

void V3OutFormatter::putns(const AstNode* nodep, const char* strg) {
    if (!v3Global.opt.decoration()) {
        putsOutput(strg);
        return;
    }

    if (m_prependIndent && strg[0] != '\n') {
        putsNoTracking(indentSpaces(endLevels(strg)));
        m_prependIndent = false;
    }

    if (nodep && v3Global.opt.decorationNodes() && !v3Global.opt.protectIds()
        && (m_sourceLastFilenameno != nodep->fileline()->filenameno()
            || m_sourceLastLineno != nodep->fileline()->firstLineno())
        && FileLine::builtInFilename() != nodep->fileline()->filename()) {
        m_sourceLastLineno = nodep->fileline()->firstLineno();
        m_sourceLastFilenameno = nodep->fileline()->filenameno();
        putsNoTracking("/*" + nodep->fileline()->filename() + ":"
                       + cvtToStr(nodep->fileline()->lineno()) + " " + cvtToStr((void*)nodep)
                       + "*/");
    }

    bool notstart = false;
    bool wordstart = true;
    bool equalsForBracket = false;  // Looking for "= {"
    for (const char* cp = strg; *cp; ++cp) {
        putcNoTracking(*cp);
        if (std::isalpha(*cp)) {
            if (wordstart && m_lang == LA_VERILOG && tokenNotStart(cp)) notstart = true;
            if (wordstart && m_lang == LA_VERILOG && !notstart && tokenStart(cp)) indentInc();
            if (wordstart && m_lang == LA_VERILOG && tokenEnd(cp)) indentDec();
        }
        switch (*cp) {
        case '\n':
            ++m_lineno;
            wordstart = true;
            if (cp[1] == '\0') {
                // Add the indent later, may be a indentInc/indentDec
                // called between now and then
                m_prependIndent = true;
            } else {
                m_prependIndent = false;
                putsNoTracking(indentSpaces(endLevels(cp + 1)));
            }
            break;
        case ' ': wordstart = true; break;
        case '\t': wordstart = true; break;
        case '"':
            wordstart = false;
            m_inStringLiteral = !m_inStringLiteral;
            break;
        case '/':
            if (m_lang == LA_C || m_lang == LA_VERILOG) {
                if (cp > strg && cp[-1] == '/' && !m_inStringLiteral) {
                    // Output ignoring contents to EOL
                    ++cp;
                    while (*cp && cp[1] && cp[1] != '\n') putcNoTracking(*cp++);
                    if (*cp) putcNoTracking(*cp);
                }
            }
            break;
        case '{':
            if (m_lang == LA_C && (equalsForBracket || m_bracketLevel)) {
                // Break up large code inside "= { ..."
                m_parenVec.push(m_indentLevel
                                * m_blockIndent);  // Line up continuation with block+1
                ++m_bracketLevel;
            }
            indentInc();
            break;
        case '}':
            if (m_bracketLevel > 0) {
                m_parenVec.pop();
                --m_bracketLevel;
            }
            indentDec();
            break;
        case '(':
            indentInc();
            // Line up continuation with open paren, plus one indent
            m_parenVec.push(m_column);
            break;
        case ')':
            if (!m_parenVec.empty()) m_parenVec.pop();
            indentDec();
            break;
        case '<':
            if (m_lang == LA_XML) {
                if (cp[1] == '/') {
                    // Zero as the > will result in net decrease by one
                } else if (cp[1] == '!' || cp[1] == '?') {
                    indentInc();  // net same indent
                } else {
                    indentInc();  // net increase by one
                    indentInc();
                }
            }
            break;
        case '>':
            if (m_lang == LA_XML) {
                indentDec();
                if (cp > strg && cp[-1] == '/') indentDec();  // < ..... /> stays same level
            }
            break;
        default: wordstart = false; break;
        }

        switch (*cp) {
        case '=': equalsForBracket = true; break;
        case ' ': break;
        default: equalsForBracket = false; break;
        }
    }
}

void V3OutFormatter::putBreakExpr() {
    if (!m_parenVec.empty()) putBreak();
}

// Add a line break if too wide
void V3OutFormatter::putBreak() {
    if (!v3Global.opt.decoration()) return;
    if (!m_nobreak) {
        // char s[1000]; sprintf(s, "{%d,%d}", m_column, m_parenVec.top()); putsNoTracking(s);
        if (exceededWidth()) {
            putcNoTracking('\n');
            if (!m_parenVec.empty()) putsNoTracking(indentSpaces(m_parenVec.top()));
        }
    }
}

void V3OutFormatter::putsQuoted(const string& strg) {
    // Quote \ and " for use inside C programs
    // Don't use to quote a filename for #include - #include doesn't \ escape.
    const string quoted = quoteNameControls(strg);
    putcNoTracking('"');
    for (const char c : quoted) putcNoTracking(c);
    putcNoTracking('"');
    if (strg.find('\0') != std::string::npos) putcNoTracking('s');  // C++14 std::string
}
void V3OutFormatter::putsNoTracking(const string& strg) {
    if (!v3Global.opt.decoration()) {
        putsOutput(strg.c_str());
        return;
    }
    // Don't track {}'s, probably because it's a $display format string
    for (const char c : strg) putcNoTracking(c);
}

void V3OutFormatter::putcNoTracking(char chr) {
    if (!v3Global.opt.decoration()) {
        putcOutput(chr);
        return;
    }
    switch (chr) {
    case '\n':
        ++m_lineno;
        m_column = 0;
        m_nobreak = true;
        break;
    case '\t': m_column = ((m_column + 9) / 8) * 8; break;
    case ' ':
    case '(':
    case '|':
    case '&': ++m_column; break;
    default:
        ++m_column;
        m_nobreak = false;
        break;
    }
    putcOutput(chr);
}

string V3OutFormatter::quoteNameControls(const string& namein,
                                         V3OutFormatter::Language lang) VL_PURE {
    // Encode control chars into output-appropriate escapes
    // Reverse is VString::unquoteSVString
    string out;
    if (lang == LA_XML) {
        // Encode chars into XML string
        for (const char c : namein) {
            if (c == '"') {
                out += "&quot;"s;
            } else if (c == '\'') {
                out += "&apos;"s;
            } else if (c == '<') {
                out += "&lt;"s;
            } else if (c == '>') {
                out += "&gt;"s;
            } else if (c == '&') {
                out += "&amp;"s;
            } else if (std::isprint(c)) {
                out += c;
            } else {
                out += "&#"s + cvtToStr((unsigned int)(c & 0xff)) + ";";
            }
        }
    } else {
        // Encode control chars into C style escapes
        for (const char c : namein) {
            if (c == '\\' || c == '"') {
                out += "\\"s + c;
            } else if (c == '\n') {
                out += "\\n";
            } else if (c == '\r') {
                out += "\\r";
            } else if (c == '\t') {
                out += "\\t";
            } else if (std::isprint(c)) {
                out += c;
            } else {
                // This will also cover \a etc
                const string octal
                    = "\\"s + cvtToStr((c >> 6) & 3) + cvtToStr((c >> 3) & 7) + cvtToStr(c & 7);
                out += octal;
            }
        }
    }
    return out;
}

//----------------------------------------------------------------------
// Simple wrappers

void V3OutFormatter::printf(const char* fmt...) {
    constexpr size_t bufsize = 5000;
    char sbuff[bufsize];
    va_list ap;
    va_start(ap, fmt);
    VL_VSNPRINTF(sbuff, bufsize, fmt, ap);
    va_end(ap);
    this->puts(sbuff);
}

//######################################################################
// V3OutFormatter: A class for printing to a file, with automatic indentation of C++ code.

V3OutFile::V3OutFile(const string& filename, V3OutFormatter::Language lang)
    : V3OutFormatter{lang}
    , m_filename{filename}
    , m_bufferp{new std::array<char, WRITE_BUFFER_SIZE_BYTES>{}} {
    if ((m_fp = V3File::new_fopen_w(filename)) == nullptr) {
        v3fatal("Can't write file: " << filename);
    }
}

V3OutFile::~V3OutFile() {
    writeBlock();

    if (m_fp) fclose(m_fp);
    m_fp = nullptr;
}

void V3OutFile::putsForceIncs() {
    const V3StringList& forceIncs = v3Global.opt.forceIncs();
    for (const string& i : forceIncs) puts("#include \"" + i + "\"\n");
}

void V3OutCFile::putsGuard() {
    UASSERT(!m_guard, "Already called putsGuard in emit file");
    m_guard = true;
    string var = VString::upcase("VERILATED_"s + V3Os::filenameNonDir(filename()) + "_");
    for (char& c : var) {
        if (!std::isalnum(c)) c = '_';
    }
    puts("\n#ifndef " + var + "\n");
    puts("#define " + var + "  // guard\n");
}

//######################################################################
// V3OutStream

V3OutStream::V3OutStream(std::ostream& ostream, V3OutFormatter::Language lang)
    : V3OutFormatter{lang}
    , m_ostream{ostream} {}

//######################################################################
// VIdProtect

class VIdProtectImp final {
    // MEMBERS
    V3Mutex m_mutex;  // Protects members
    std::map<const std::string, std::string> m_nameMap;  // Map of old name into new name
    std::unordered_set<std::string> m_newIdSet VL_GUARDED_BY(m_mutex);  // Which new names exist
protected:
    // CONSTRUCTORS
    friend class VIdProtect;
    static VIdProtectImp& singleton() VL_MT_SAFE {
        static VIdProtectImp s;
        return s;
    }

public:
    VIdProtectImp() {
        passthru("this");
        passthru("TOP");
        passthru("vlSelf");
        passthru("vlSymsp");
    }
    ~VIdProtectImp() = default;
    // METHODS
    string passthru(const string& old) VL_MT_SAFE_EXCLUDES(m_mutex) {
        if (!v3Global.opt.protectIds()) return old;
        const V3LockGuard lock{m_mutex};
        const auto it = m_nameMap.find(old);
        if (it != m_nameMap.end()) {
            // No way to go back and correct the older crypt name
            UASSERT(old == it->second,
                    "Pass-thru request for '" + old + "' after already --protect-ids of it.");
        } else {
            m_nameMap.emplace(old, old);
            m_newIdSet.insert(old);
        }
        return old;
    }
    string protectIf(const string& old, bool doIt) VL_MT_SAFE_EXCLUDES(m_mutex) {
        if (!v3Global.opt.protectIds() || old.empty() || !doIt) return old;
        const V3LockGuard lock{m_mutex};
        const auto pair = m_nameMap.emplace(old, "");
        if (pair.second) {
            string out;
            if (v3Global.opt.debugProtect()) {
                // This lets us see the symbol being protected to debug cases
                // where e.g. the definition is protect() but reference is
                // missing a protect()
                out = "PS" + old;
            } else {
                VHashSha256 digest{v3Global.opt.protectKeyDefaulted()};
                digest.insert(old);
                // Add "PS" prefix (Protect Symbols) as cannot start symbol with number
                out = "PS" + digest.digestSymbol();
                // See if we can shrink the digest symbol to something smaller
                for (size_t len = 6; len < out.size() - 3; len += 3) {
                    const string tryout = out.substr(0, len);
                    if (m_newIdSet.insert(tryout).second) {
                        out = tryout;
                        break;
                    }
                }
            }
            pair.first->second = out;
        }
        return pair.first->second;
    }
    string protectWordsIf(const string& old, bool doIt) VL_MT_SAFE {
        // Split at " " (for traces), "." (for scopes), "->", "(", "&", ")" (for self pointers)
        if (!(doIt && v3Global.opt.protectIds())) return old;
        string out;
        string::size_type start = 0;
        // space, ., ->
        while (true) {
            // When C++11, use find_if and lambda
            string::size_type pos = string::npos;
            string separator;
            trySep(old, start, " ", pos /*ref*/, separator /*ref*/);
            trySep(old, start, ".", pos /*ref*/, separator /*ref*/);
            trySep(old, start, "->", pos /*ref*/, separator /*ref*/);
            trySep(old, start, "(", pos /*ref*/, separator /*ref*/);
            trySep(old, start, "&", pos /*ref*/, separator /*ref*/);
            trySep(old, start, ")", pos /*ref*/, separator /*ref*/);
            if (pos == string::npos) break;
            out += protectIf(old.substr(start, pos - start), true) + separator;
            start = pos + separator.length();
        }
        out += protectIf(old.substr(start), true);
        return out;
    }
    void writeMapFile(const string& filename) const {
        V3OutXmlFile of{filename};
        of.putsHeader();
        of.puts("<!-- DESCR"
                "IPTION: Verilator output: XML representation of netlist -->\n");
        of.puts("<verilator_id_map>\n");
        {
            for (const auto& itr : m_nameMap) {
                of.puts("<map from=\"" + itr.second + "\" to=\"" + itr.first + "\"/>\n");
            }
        }
        of.puts("</verilator_id_map>\n");
    }

private:
    void trySep(const string& old, string::size_type start, const string& trySep,
                string::size_type& posr, string& separatorr) VL_PURE {
        const string::size_type trypos = old.find(trySep, start);
        if (trypos != string::npos) {
            if (posr == string::npos || (posr > trypos)) {
                posr = trypos;
                separatorr = trySep;
            }
        }
    }
};

string VIdProtect::protectIf(const string& old, bool doIt) VL_MT_SAFE {
    return VIdProtectImp::singleton().protectIf(old, doIt);
}
string VIdProtect::protectWordsIf(const string& old, bool doIt) VL_MT_SAFE {
    return VIdProtectImp::singleton().protectWordsIf(old, doIt);
}
void VIdProtect::writeMapFile(const string& filename) {
    VIdProtectImp::singleton().writeMapFile(filename);
}
