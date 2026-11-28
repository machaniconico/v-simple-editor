#include "HangDump.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

namespace {

std::atomic<bool> g_finished{false};
std::once_flag g_deadlineOnce;
HANDLE g_dumpStartedEvent = nullptr;
DWORD g_deadlineThreadId = 0;
std::once_flag g_symbolsOnce;
bool g_symbolsReady = false;

DWORD WINAPI dumpDeadline(LPVOID)
{
    if (WaitForSingleObject(g_dumpStartedEvent, INFINITE) != WAIT_OBJECT_0)
        return 0;
    Sleep(10000);
    if (!g_finished.load(std::memory_order_acquire)) {
        static constexpr char kDeadlineLine[] = "watchdog: dump deadline elapsedMs=10000\n";
        DWORD written = 0;
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), kDeadlineLine,
                  static_cast<DWORD>(sizeof(kDeadlineLine) - 1), &written, nullptr);
        TerminateProcess(GetCurrentProcess(), 124);
    }
    return 0;
}

void writeLine(HANDLE file, const char* line)
{
    DWORD written = 0;
    WriteFile(file, line, static_cast<DWORD>(std::strlen(line)), &written, nullptr);
}

void widenAscii(const char* source, wchar_t* destination, size_t capacity)
{
    size_t i = 0;
    while (source[i] && i + 1 < capacity) {
        destination[i] = static_cast<unsigned char>(source[i]);
        ++i;
    }
    destination[i] = L'\0';
}

void walkThread(HANDLE file, HANDLE process, HANDLE thread, DWORD tid,
                CONTEXT& context, bool selfThread)
{
    char line[1024];
    std::snprintf(line, sizeof(line), "thread %lu%s\r\n",
                  static_cast<unsigned long>(tid), selfThread ? " (watchdog)" : "");
    writeLine(file, line);

    STACKFRAME64 frame{};
    DWORD machine = 0;
#if defined(_M_X64)
    machine = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset = context.Rip;
    // x64 Release builds commonly omit a frame pointer; StackWalk64 uses
    // unwind metadata with RSP as the initial frame address.
    frame.AddrFrame.Offset = context.Rsp;
    frame.AddrStack.Offset = context.Rsp;
#elif defined(_M_IX86)
    machine = IMAGE_FILE_MACHINE_I386;
    frame.AddrPC.Offset = context.Eip;
    frame.AddrFrame.Offset = context.Ebp;
    frame.AddrStack.Offset = context.Esp;
#else
    writeLine(file, "  stack walk unsupported on this architecture\r\n");
    return;
#endif
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Mode = AddrModeFlat;

    alignas(SYMBOL_INFO) char symbolStorage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME]{};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolStorage);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;
    const CONTEXT initialContext = context;
    DWORD64 previousPc = 0;
    int walked = 0;
    for (int depth = 0; depth < 128 && frame.AddrPC.Offset; ++depth) {
        const DWORD64 pc = frame.AddrPC.Offset;
        if (pc == previousPc) break;
        ++walked;
        previousPc = pc;
        DWORD64 displacement = 0;
        if (SymFromAddr(process, pc, &displacement, symbol))
            std::snprintf(line, sizeof(line), "  %02d 0x%llx %s+0x%llx\r\n", depth,
                          static_cast<unsigned long long>(pc), symbol->Name,
                          static_cast<unsigned long long>(displacement));
        else
            std::snprintf(line, sizeof(line), "  %02d 0x%llx\r\n", depth,
                          static_cast<unsigned long long>(pc));
        writeLine(file, line);
        if (!StackWalk64(machine, process, thread, &frame, &context, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) {
            std::snprintf(line, sizeof(line), "  stack walk stopped error=%lu\r\n",
                          static_cast<unsigned long>(GetLastError()));
            writeLine(file, line);
            break;
        }
    }

#if defined(_M_X64)
    // Some optimized Windows stacks make StackWalk64 return a zero PC after
    // its first frame. Keep the required StackWalk64 pass above, then use the
    // x64 unwind metadata to recover the callers in this same process.
    if (walked < 2) {
        using LookupFn = PRUNTIME_FUNCTION (NTAPI*)(DWORD64, PDWORD64, void*);
        using UnwindFn = PVOID (NTAPI*)(DWORD, DWORD64, DWORD64,
            PRUNTIME_FUNCTION, PCONTEXT, PVOID*, PDWORD64, void*);
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        const auto lookup = ntdll ? reinterpret_cast<LookupFn>(
            GetProcAddress(ntdll, "RtlLookupFunctionEntry")) : nullptr;
        const auto unwind = ntdll ? reinterpret_cast<UnwindFn>(
            GetProcAddress(ntdll, "RtlVirtualUnwind")) : nullptr;
        if (lookup && unwind) {
            CONTEXT current = initialContext;
            for (int depth = 1; depth < 128; ++depth) {
                const DWORD64 oldPc = current.Rip;
                const DWORD64 oldSp = current.Rsp;
                DWORD64 imageBase = 0;
                PRUNTIME_FUNCTION function = lookup(current.Rip, &imageBase, nullptr);
                if (function) {
                    PVOID handlerData = nullptr;
                    DWORD64 establisherFrame = 0;
                    unwind(0, imageBase, current.Rip, function, &current,
                           &handlerData, &establisherFrame, nullptr);
                } else {
                    DWORD64 returnAddress = 0;
                    SIZE_T bytesRead = 0;
                    if (!ReadProcessMemory(process,
                            reinterpret_cast<const void*>(current.Rsp),
                            &returnAddress, sizeof(returnAddress), &bytesRead) ||
                        bytesRead != sizeof(returnAddress))
                        break;
                    current.Rip = returnAddress;
                    current.Rsp += sizeof(returnAddress);
                }
                if (!current.Rip || current.Rip == oldPc || current.Rsp <= oldSp)
                    break;
                DWORD64 displacement = 0;
                if (SymFromAddr(process, current.Rip, &displacement, symbol))
                    std::snprintf(line, sizeof(line), "  %02d 0x%llx %s+0x%llx\r\n", depth,
                                  static_cast<unsigned long long>(current.Rip), symbol->Name,
                                  static_cast<unsigned long long>(displacement));
                else
                    std::snprintf(line, sizeof(line), "  %02d 0x%llx\r\n", depth,
                                  static_cast<unsigned long long>(current.Rip));
                writeLine(file, line);
            }
        }
    }
#endif
}

bool writeStacks(const wchar_t* path)
{
    const HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const HANDLE process = GetCurrentProcess();
    const DWORD pid = GetCurrentProcessId();
    const DWORD selfTid = GetCurrentThreadId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    bool enumerated = false;
    if (snapshot != INVALID_HANDLE_VALUE) {
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        if (Thread32First(snapshot, &entry)) {
            do {
                if (entry.th32OwnerProcessID != pid) continue;
                // The deadline must remain runnable if stack walking blocks.
                if (entry.th32ThreadID == g_deadlineThreadId) continue;
                enumerated = true;
                if (entry.th32ThreadID == selfTid) {
                    CONTEXT context{};
                    RtlCaptureContext(&context);
                    walkThread(file, process, GetCurrentThread(), selfTid, context, true);
                    continue;
                }
                HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                           THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
                if (!thread) continue;
                if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
                    CONTEXT context{};
                    context.ContextFlags = CONTEXT_FULL;
                    if (GetThreadContext(thread, &context))
                        walkThread(file, process, thread, entry.th32ThreadID, context, false);
                    ResumeThread(thread);
                }
                CloseHandle(thread);
            } while (Thread32Next(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }
    const bool flushed = FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    return enumerated && flushed;
}

} // namespace
#endif

namespace hangdump {

void prepare()
{
#ifdef _WIN32
    std::call_once(g_deadlineOnce, []() {
        g_dumpStartedEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!g_dumpStartedEvent) return;
        HANDLE deadline = CreateThread(nullptr, 0, dumpDeadline, nullptr, 0,
                                       &g_deadlineThreadId);
        if (deadline) CloseHandle(deadline);
        else {
            CloseHandle(g_dumpStartedEvent);
            g_dumpStartedEvent = nullptr;
        }
    });
#endif
}

Result writeHangArtifacts(const char* selftestName)
{
    Result result;
#ifdef _WIN32
    const auto started = std::chrono::steady_clock::now();
    g_finished.store(false, std::memory_order_release);
    if (g_dumpStartedEvent) SetEvent(g_dumpStartedEvent);

    CreateDirectoryW(L".omc", nullptr);
    CreateDirectoryW(L".omc\\artifacts", nullptr);
    CreateDirectoryW(L".omc\\artifacts\\hang-dumps", nullptr);

    char safeName[96]{};
    const char* input = selftestName ? selftestName : "unknown";
    size_t length = 0;
    while (*input && length < sizeof(safeName) - 1) {
        const char c = *input++;
        safeName[length++] = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                             (c >= '0' && c <= '9') || c == '-' ? c : '_';
    }
    if (!length) std::strcpy(safeName, "unknown");

    SYSTEMTIME utc{};
    GetSystemTime(&utc);
    char stem[256]{};
    std::snprintf(stem, sizeof(stem), ".omc/artifacts/hang-dumps/%s-%lu-%04u%02u%02u-%02u%02u%02u-%03u",
                  safeName, static_cast<unsigned long>(GetCurrentProcessId()),
                  utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond,
                  utc.wMilliseconds);
    std::snprintf(result.dumpPath, sizeof(result.dumpPath), "%s.dmp", stem);
    wchar_t widePath[512]{};
    widenAscii(result.dumpPath, widePath, 512);

    std::call_once(g_symbolsOnce, []() {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        g_symbolsReady = SymInitialize(GetCurrentProcess(), nullptr, TRUE) != FALSE;
    });

    HANDLE file = CreateFileW(widePath, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        const bool generated = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
            static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo),
            nullptr, nullptr, nullptr) != FALSE;
        result.dumpWritten = generated && FlushFileBuffers(file) != FALSE;
        CloseHandle(file);
        if (!result.dumpWritten) DeleteFileW(widePath);
    }
    if (result.dumpWritten) {
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        std::fprintf(stderr, "watchdog: minidump ready %s elapsedMs=%lld\n",
                     result.dumpPath, static_cast<long long>(elapsedMs));
        std::fflush(stderr);
    }
    if (result.dumpWritten && g_symbolsReady) {
        char textPath[512]{};
        std::snprintf(textPath, sizeof(textPath), "%s.txt", stem);
        wchar_t wideTextPath[512]{};
        widenAscii(textPath, wideTextPath, 512);
        result.stacksWritten = writeStacks(wideTextPath);
    }
    result.elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    g_finished.store(true, std::memory_order_release);
#else
    (void)selftestName;
#endif
    return result;
}

} // namespace hangdump
