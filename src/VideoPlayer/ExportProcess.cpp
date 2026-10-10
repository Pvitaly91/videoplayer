#include "targetver.h"
#include "ExportProcess.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "MediaOpen.h"

#include <algorithm>
#include <array>
#include <thread>

namespace videoplayer {
namespace {

class Handle final {
public:
    Handle() = default;
    explicit Handle(HANDLE value) : value_(value) {}
    ~Handle() { Reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE Get() const noexcept { return value_; }
    HANDLE Release() noexcept { HANDLE value = value_; value_ = nullptr; return value; }
    void Reset(HANDLE value = nullptr) noexcept {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
        value_ = value;
    }
private:
    HANDLE value_ = nullptr;
};

std::wstring LastErrorText(const wchar_t* operation) {
    return std::wstring(operation) + L" (Win32 " + std::to_wstring(GetLastError()) + L").";
}

bool MakePipe(Handle& read, Handle& write, std::wstring& error) {
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE rawRead = nullptr;
    HANDLE rawWrite = nullptr;
    if (CreatePipe(&rawRead, &rawWrite, &security, 0) == FALSE) {
        error = LastErrorText(L"CreatePipe не виконано");
        return false;
    }
    read.Reset(rawRead);
    write.Reset(rawWrite);
    return true;
}

void AppendBounded(std::string& destination,
    const char* data, const std::size_t size, const std::size_t limit) {
    if (size == 0 || limit == 0) return;
    if (size >= limit) {
        destination.assign(data + size - limit, data + size);
        return;
    }
    if (destination.size() + size > limit) {
        destination.erase(0, destination.size() + size - limit);
    }
    destination.append(data, size);
}

void DrainPipe(const HANDLE pipe, std::string& output,
    const std::size_t limit, const ExportProgressLine& progress) {
    std::array<char, 4096> buffer{};
    std::string pending;
    DWORD read = 0;
    while (ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) != FALSE && read != 0) {
        AppendBounded(output, buffer.data(), read, limit);
        if (progress) {
            pending.append(buffer.data(), read);
            for (;;) {
                const auto lineEnd = pending.find('\n');
                if (lineEnd == std::string::npos) break;
                std::string line = pending.substr(0, lineEnd);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                progress(line);
                pending.erase(0, lineEnd + 1);
            }
            if (pending.size() > 65536U) pending.erase(0, pending.size() - 65536U);
        }
    }
    if (progress && !pending.empty()) progress(pending);
}

}  // namespace

std::wstring ExportToolsDirectory() {
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const DWORD copied = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (copied == 0) return {};
        if (copied + 1 < buffer.size()) {
            std::wstring path(buffer.data(), copied);
            const auto separator = path.find_last_of(L"\\/");
            return (separator == std::wstring::npos ? L"." : path.substr(0, separator)) +
                L"\\tools\\export";
        }
        if (buffer.size() >= 32768U) return {};
        buffer.resize((std::min<std::size_t>)(32768U, buffer.size() * 2U));
    }
}

ExportProcessResult RunHiddenExportProcess(
    const std::wstring& executable,
    const std::vector<std::wstring>& arguments,
    std::atomic_bool* cancellation,
    ExportProgressLine progress,
    const std::size_t outputLimit,
    const std::string& standardInput) {
    ExportProcessResult result{};
    const bool absolute = executable.size() >= 3 &&
        ((executable[1] == L':' && (executable[2] == L'\\' || executable[2] == L'/')) ||
         (executable[0] == L'\\' && executable[1] == L'\\'));
    if (!absolute || GetFileAttributesW(executable.c_str()) == INVALID_FILE_ATTRIBUTES) {
        result.error = L"Bundled executable не знайдено: " + executable;
        return result;
    }
    Handle stdoutRead, stdoutWrite, stderrRead, stderrWrite, stdinRead, stdinWrite;
    if (!MakePipe(stdoutRead, stdoutWrite, result.error) ||
        !MakePipe(stderrRead, stderrWrite, result.error) ||
        !MakePipe(stdinRead, stdinWrite, result.error)) return result;
    SetHandleInformation(stdoutRead.Get(), HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stderrRead.Get(), HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stdinWrite.Get(), HANDLE_FLAG_INHERIT, 0);

    std::wstring command = QuoteWindowsCommandLineArgument(executable);
    for (const auto& argument : arguments) {
        command += L" ";
        command += QuoteWindowsCommandLineArgument(argument);
    }
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.StartupInfo.wShowWindow = SW_HIDE;
    startup.StartupInfo.hStdInput = stdinRead.Get();
    startup.StartupInfo.hStdOutput = stdoutWrite.Get();
    startup.StartupInfo.hStdError = stderrWrite.Get();
    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    std::vector<unsigned char> attributeStorage(attributeBytes);
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
    if (attributeBytes == 0 || InitializeProcThreadAttributeList(
            startup.lpAttributeList, 1, 0, &attributeBytes) == FALSE) {
        result.error = LastErrorText(L"InitializeProcThreadAttributeList не виконано");
        return result;
    }
    const std::array<HANDLE, 3> inherited{
        stdinRead.Get(), stdoutWrite.Get(), stderrWrite.Get()};
    if (UpdateProcThreadAttribute(startup.lpAttributeList, 0,
            PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            const_cast<HANDLE*>(inherited.data()), sizeof(inherited), nullptr, nullptr) == FALSE) {
        result.error = LastErrorText(L"UpdateProcThreadAttribute не виконано");
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        return result;
    }
    PROCESS_INFORMATION process{};
    Handle job(CreateJobObjectW(nullptr, nullptr));
    if (job.Get() == nullptr) {
        result.error = LastErrorText(L"CreateJobObjectW не виконано");
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        return result;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (SetInformationJobObject(job.Get(), JobObjectExtendedLimitInformation,
            &limits, sizeof(limits)) == FALSE) {
        result.error = LastErrorText(L"SetInformationJobObject не виконано");
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        return result;
    }
    if (CreateProcessW(executable.c_str(), mutableCommand.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
            nullptr, nullptr, &startup.StartupInfo, &process) == FALSE) {
        result.error = LastErrorText(L"CreateProcessW не виконано");
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        return result;
    }
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    Handle processHandle(process.hProcess);
    Handle threadHandle(process.hThread);
    if (AssignProcessToJobObject(job.Get(), processHandle.Get()) == FALSE) {
        TerminateProcess(processHandle.Get(), ERROR_ACCESS_DENIED);
        result.error = LastErrorText(L"AssignProcessToJobObject не виконано");
        return result;
    }
    result.started = true;
    ResumeThread(threadHandle.Get());
    stdoutWrite.Reset();
    stderrWrite.Reset();
    stdinRead.Reset();

    std::thread stdoutThread(DrainPipe, stdoutRead.Get(), std::ref(result.standardOutput),
        outputLimit, std::cref(progress));
    std::thread stderrThread(DrainPipe, stderrRead.Get(), std::ref(result.standardError),
        outputLimit, ExportProgressLine{});
    std::thread inputThread;
    if (!standardInput.empty()) {
        inputThread = std::thread([&stdinWrite, &standardInput] {
            std::size_t offset = 0;
            while (offset < standardInput.size()) {
                DWORD written = 0;
                const DWORD count = static_cast<DWORD>((std::min<std::size_t>)(
                    standardInput.size() - offset, 4096U));
                if (WriteFile(stdinWrite.Get(), standardInput.data() + offset,
                        count, &written, nullptr) == FALSE || written == 0) break;
                offset += written;
            }
            stdinWrite.Reset();
        });
    }
    bool quitSent = false;
    ULONGLONG quitAt = 0;
    for (;;) {
        const DWORD wait = WaitForSingleObject(processHandle.Get(), 50);
        if (wait == WAIT_OBJECT_0) break;
        if (wait == WAIT_FAILED) {
            TerminateJobObject(job.Get(), ERROR_PROCESS_ABORTED);
            break;
        }
        if (cancellation != nullptr && cancellation->load()) {
            result.cancelled = true;
            if (!quitSent) {
                if (standardInput.empty()) {
                    static constexpr char quit[] = "q\n";
                    DWORD written = 0;
                    WriteFile(stdinWrite.Get(), quit, 2, &written, nullptr);
                }
                quitSent = true;
                quitAt = GetTickCount64();
            } else if (GetTickCount64() - quitAt >= 2000) {
                TerminateJobObject(job.Get(), ERROR_CANCELLED);
            }
        }
    }
    if (inputThread.joinable()) inputThread.join();
    stdinWrite.Reset();
    stdoutThread.join();
    stderrThread.join();
    GetExitCodeProcess(processHandle.Get(), &result.exitCode);
    return result;
}

}  // namespace videoplayer
