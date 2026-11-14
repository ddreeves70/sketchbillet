#include "CrashReport.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QRegularExpression>
#include <QSysInfo>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <execinfo.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace sb::app::crash {

namespace {

// Everything the crash handlers use is made ready here, before any crash, so a
// handler only copies bytes into a file: after a crash, memory may be broken, so
// it must not allocate, lock or call Qt.
std::string header;  // "SketchBillet crash report\nversion=...\n..."
#ifdef _WIN32
std::wstring path;
#else
std::string path;
#endif
char lastThing[128] = "";
char exceptionText[256] = "";
std::atomic<bool> crashing{false};

// Copies `text` into a fixed buffer, keeping it to one line of printable characters.
template <std::size_t N>
void keep(char (&buffer)[N], std::string_view text) {
    std::size_t n = 0;
    for (const char c : text) {
        if (n + 1 >= N) break;
        buffer[n++] = (c >= 32 && c < 127) ? c : '?';
    }
    buffer[n] = '\0';
}

#ifdef _WIN32
using File = HANDLE;
File openReport() {
    return CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                       nullptr);
}
bool good(File f) {
    return f != INVALID_HANDLE_VALUE;
}
void put(File f, const char* text, std::size_t n) {
    DWORD wrote = 0;
    WriteFile(f, text, static_cast<DWORD>(n), &wrote, nullptr);
}
void closeReport(File f) {
    CloseHandle(f);
}
#else
using File = int;
File openReport() {
    return ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
}
bool good(File f) {
    return f >= 0;
}
void put(File f, const char* text, std::size_t n) {
    while (n > 0) {
        const auto w = ::write(f, text, n);
        if (w <= 0) return;
        text += w;
        n -= static_cast<std::size_t>(w);
    }
}
void closeReport(File f) {
    ::close(f);
}
#endif

void put(File f, const char* text) {
    put(f, text, std::strlen(text));
}

void putNumber(File f, unsigned long long n, unsigned base = 10) {
    char digits[32];
    int i = 0;
    do {
        const auto d = static_cast<unsigned>(n % base);
        digits[i++] = static_cast<char>(d < 10 ? '0' + d : 'a' + d - 10);
        n /= base;
    } while (n > 0 && i < 32);
    char out[34];
    int o = 0;
    if (base == 16) {
        out[o++] = '0';
        out[o++] = 'x';
    }
    while (i > 0) out[o++] = digits[--i];
    put(f, out, static_cast<std::size_t>(o));
}

// Writes the report: what's known about this copy, when, what went wrong, the
// last thing clicked, then the stack (`stack` writes it).
template <typename Stack>
void writeReport(const char* what, unsigned long long code, Stack stack) {
    if (crashing.exchange(true)) return;  // a second crash while writing the first
    const File f = openReport();
    if (!good(f)) return;
    put(f, header.data(), header.size());
    put(f, "time=");
    putNumber(f, static_cast<unsigned long long>(std::time(nullptr)));
    put(f, "\nwhat=");
    put(f, what);
    if (code != 0) {
        put(f, " ");
        putNumber(f, code, 16);
    }
    if (exceptionText[0] != '\0') {
        put(f, "\nexception=");
        put(f, exceptionText);
    }
    put(f, "\nlast=");
    put(f, lastThing);
    put(f, "\nstack:\n");
    stack(f);
    closeReport(f);
}

#ifdef _WIN32
void writeStack(File f) {
    void* frames[64];
    const USHORT n = RtlCaptureStackBackTrace(0, 64, frames, nullptr);
    for (USHORT i = 0; i < n; ++i) {
        HMODULE module = nullptr;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                static_cast<LPCSTR>(frames[i]), &module)) {
            GetModuleFileNameA(module, name, MAX_PATH);
        }
        const char* base = name;
        for (const char* p = name; *p; ++p) {
            if (*p == '\\' || *p == '/') base = p + 1;
        }
        put(f, base);
        put(f, "+");
        putNumber(
            f, reinterpret_cast<unsigned long long>(frames[i]) - reinterpret_cast<unsigned long long>(module),
            16);
        put(f, "\n");
    }
}

LPTOP_LEVEL_EXCEPTION_FILTER runtimeFilter = nullptr;

LONG WINAPI onException(EXCEPTION_POINTERS* info) {
    // A C++ exception nobody caught: the C++ runtime's own filter calls
    // std::terminate, and onTerminate writes the report with its message.
    constexpr DWORD kCppException = 0xE06D7363;
    if (info->ExceptionRecord->ExceptionCode == kCppException && runtimeFilter) return runtimeFilter(info);
    writeReport("exception-code", info->ExceptionRecord->ExceptionCode, writeStack);
    return EXCEPTION_EXECUTE_HANDLER;  // end the program now, without Windows' crash dialog
}
#else
void writeStack(File f) {
    void* frames[64];
    const int n = backtrace(frames, 64);
    backtrace_symbols_fd(frames, n, f);
}
#endif

const char* signalName(int sig) {
    switch (sig) {
        case SIGSEGV:
            return "segfault";
        case SIGABRT:
            return exceptionText[0] != '\0' ? "exception" : "abort";
        case SIGFPE:
            return "arithmetic";
        case SIGILL:
            return "illegal-instruction";
#ifdef SIGBUS
        case SIGBUS:
            return "bus-error";
#endif
        default:
            return "signal";
    }
}

extern "C" void onSignal(int sig) {
    writeReport(signalName(sig), 0, writeStack);
    std::signal(sig, SIG_DFL);  // then crash as the system would have
    std::raise(sig);
}

void onTerminate() {
    // An exception nobody caught: keep its message (what() is text from SketchBillet
    // or a library, never a design), then end as abort() does.
    if (const auto e = std::current_exception()) {
        try {
            std::rethrow_exception(e);
        } catch (const std::exception& ex) {
            keep(exceptionText, ex.what());
        } catch (...) {
            keep(exceptionText, "(not a std::exception)");
        }
    } else {
        keep(exceptionText, "(std::terminate called)");
    }
    std::abort();
}

QString plainWhat(const QString& what, const QString& exception) {
    if (what == QStringLiteral("segfault"))
        return QObject::tr("SketchBillet used memory it shouldn't have (a segmentation fault).");
    if (what == QStringLiteral("abort"))
        return QObject::tr("SketchBillet stopped itself because something was wrong (abort).");
    if (what == QStringLiteral("exception"))
        return QObject::tr("An error nobody caught: %1")
            .arg(exception.isEmpty() ? QStringLiteral("?") : exception);
    if (what == QStringLiteral("arithmetic"))
        return QObject::tr("A math error, like dividing by zero (SIGFPE).");
    if (what == QStringLiteral("illegal-instruction"))
        return QObject::tr("An instruction the processor can't run (SIGILL).");
    if (what == QStringLiteral("bus-error")) return QObject::tr("A bad memory address (SIGBUS).");
    // Windows' codes for the same kinds of crash.
    const auto code = what.section(QLatin1Char(' '), 1).toLower();
    if (code == QStringLiteral("0xc0000005"))
        return QObject::tr("SketchBillet used memory it shouldn't have (an access violation, 0xc0000005).");
    if (code == QStringLiteral("0xc00000fd"))
        return QObject::tr(
            "SketchBillet ran out of room to keep track of what it was doing (a stack overflow, "
            "0xc00000fd).");
    if (code == QStringLiteral("0xc0000094"))
        return QObject::tr("A math error, like dividing by zero (0xc0000094).");
    if (what.startsWith(QStringLiteral("exception-code")))
        return QObject::tr("Windows stopped SketchBillet (exception %1).")
            .arg(what.section(QLatin1Char(' '), 1));
    return what;
}

}  // namespace

void install(const QString& folder, const BuildInfo& build) {
    QDir().mkpath(folder);
    const auto file = QDir(folder).filePath(QStringLiteral("crash-%1-%2.txt")
                                                .arg(QDateTime::currentSecsSinceEpoch())
                                                .arg(QCoreApplication::applicationPid()));
#ifdef _WIN32
    path = QDir::toNativeSeparators(file).toStdWString();
#else
    path = QFile::encodeName(file).toStdString();
#endif
    auto line = [](const char* key, const QString& value) {
        auto v = value;
        v.replace(QLatin1Char('\n'), QLatin1Char(' '));
        return std::string(key) + "=" + v.toStdString() + "\n";
    };
    header = "SketchBillet crash report\n" + line("version", build.version) + line("channel", build.channel) +
             line("commit", build.commit) +
             line("system", QSysInfo::prettyProductName() + QStringLiteral(" (") +
                                QSysInfo::currentCpuArchitecture() + QStringLiteral(")")) +
             line("qt", QString::fromLatin1(qVersion()));

    std::set_terminate(onTerminate);
#ifdef _WIN32
    runtimeFilter = SetUnhandledExceptionFilter(onException);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);  // no "abort() was called" box
    std::signal(SIGABRT, onSignal);
#else
    {
        void* warm[1];
        backtrace(warm, 1);  // loads what backtrace() needs now, not during a crash
    }
    // Its own stack, so a crash from running out of stack can still be written.
    static std::string altStack(64 * 1024, '\0');
    stack_t ss{};
    ss.ss_sp = altStack.data();
    ss.ss_size = altStack.size();
    sigaltstack(&ss, nullptr);
    struct sigaction sa {};
    sa.sa_handler = onSignal;
    sa.sa_flags = SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (const int sig : {SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS}) sigaction(sig, &sa, nullptr);
#endif
}

void note(std::string_view what) {
    keep(lastThing, what);
}

Report parse(const QByteArray& raw, const QString& file) {
    Report r;
    r.file = file;
    r.raw = raw;
    QString what;
    QString exception;
    bool inStack = false;
    for (const auto& lineBytes : raw.split('\n')) {
        const auto line = QString::fromUtf8(lineBytes).trimmed();
        if (inStack) {
            // Linux writes each program's whole path, which can have the student's
            // name in it (/home/alex/...): keep only the file's own name.
            static const QRegularExpression folders(QStringLiteral(R"(^[^(\s]*/)"));
            if (!line.isEmpty()) r.stack << QString(line).remove(folders);
            continue;
        }
        if (line == QStringLiteral("stack:")) {
            inStack = true;
            continue;
        }
        const auto eq = line.indexOf(QLatin1Char('='));
        if (eq < 0) continue;
        const auto key = line.left(eq);
        const auto value = line.mid(eq + 1);
        if (key == QStringLiteral("version"))
            r.version = value;
        else if (key == QStringLiteral("system"))
            r.system = value;
        else if (key == QStringLiteral("time"))
            r.when = QDateTime::fromSecsSinceEpoch(value.toLongLong()).toUTC();
        else if (key == QStringLiteral("what"))
            what = value;
        else if (key == QStringLiteral("exception"))
            exception = value;
        else if (key == QStringLiteral("last"))
            r.last = value;
    }
    r.what = plainWhat(what, exception);
    // The text shown and sent: as written, with the stack's folders taken out.
    const auto stackAt = raw.indexOf("\nstack:\n");
    if (stackAt >= 0)
        r.raw = raw.left(stackAt) + "\nstack:\n" + r.stack.join(QLatin1Char('\n')).toUtf8() + "\n";
    return r;
}

std::vector<Report> pending(const QString& folder) {
    std::vector<Report> reports;
    const QDir dir(folder);
    for (const auto& name : dir.entryList({QStringLiteral("crash-*.txt")}, QDir::Files)) {
        QFile f(dir.filePath(name));
        // Skip anything that isn't a whole report (a crash while writing one).
        if (!f.open(QIODevice::ReadOnly) || f.size() > 1024 * 1024) continue;
        const auto raw = f.readAll();
        if (!raw.startsWith("SketchBillet crash report\n") || !raw.contains("\nstack:\n")) continue;
        reports.push_back(parse(raw, f.fileName()));
    }
    // Newest first; for crashes in the same second, the file written last.
    auto modified = [](const Report& r) { return QFileInfo(r.file).lastModified(); };
    std::sort(reports.begin(), reports.end(), [&](const Report& a, const Report& b) {
        return a.when != b.when ? a.when > b.when : modified(a) > modified(b);
    });
    return reports;
}

void forget(const QString& folder) {
    const QDir dir(folder);
    for (const auto& name : dir.entryList({QStringLiteral("crash-*.txt")}, QDir::Files)) {
        QFile::remove(dir.filePath(name));
    }
}

QString issueBody(const Report& report) {
    QStringList out;
    out << QStringLiteral(
               "**What were you doing when SketchBillet closed?** (Please write it here: which "
               "menu items or buttons you clicked, and what kind of part you were making.)")
        << QString() << QString() << QStringLiteral("---") << QString()
        << QStringLiteral(
               "SketchBillet closed unexpectedly. This report was made by SketchBillet; it has no "
               "designs or file names in it.")
        << QString() << QStringLiteral("- **Version:** %1").arg(report.version)
        << QStringLiteral("- **System:** %1").arg(report.system)
        << QStringLiteral("- **When:** %1 UTC").arg(report.when.toString(QStringLiteral("yyyy-MM-dd HH:mm")))
        << QStringLiteral("- **What went wrong:** %1").arg(report.what)
        << QStringLiteral("- **Last thing clicked:** %1")
               .arg(report.last.isEmpty() ? QStringLiteral("(nothing yet)")
                                          : QStringLiteral("`%1`").arg(report.last));
    // The details, as written, for SketchBillet's developers.
    const auto details = QString::fromUtf8(report.raw).trimmed();
    out << QString() << QStringLiteral("<details><summary>Crash report</summary>") << QString()
        << QStringLiteral("```") << details << QStringLiteral("```") << QString()
        << QStringLiteral("</details>");
    return out.join(QLatin1Char('\n'));
}

QUrl issueUrl(const Report& report) {
    const auto title =
        report.last.isEmpty()
            ? QStringLiteral("Crash in SketchBillet %1").arg(report.version)
            : QStringLiteral("Crash in SketchBillet %1 after %2").arg(report.version, report.last);
    // Browsers and GitHub accept links up to about 8,000 characters: drop stack
    // lines from the end (the outermost, least useful) until it fits.
    Report shorter = report;
    QByteArray query;
    for (;;) {
        query = "title=" + QUrl::toPercentEncoding(title) +
                "&body=" + QUrl::toPercentEncoding(issueBody(shorter));
        if (query.size() <= 7000) break;
        const auto lines = shorter.raw.split('\n');
        if (lines.size() <= 12) break;
        shorter.raw = lines.mid(0, lines.size() - 2).join('\n');
    }
    return QUrl::fromEncoded("https://github.com/ddreeves70/sketchbillet/issues/new?" + query);
}

void crashForTest(const QString& how) {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
    if (how == QStringLiteral("abort")) std::abort();
    if (how == QStringLiteral("exception")) throw std::runtime_error("a test crash, from --crash-for-test");
    volatile int* nowhere = nullptr;
    *nowhere = 1;  // a bad memory write
    std::abort();
}

}  // namespace sb::app::crash
