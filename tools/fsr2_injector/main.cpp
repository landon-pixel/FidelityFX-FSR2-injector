#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace
{
struct Options
{
    bool list = false;
    bool dryRun = false;
    bool waitForExit = false;
    DWORD pid = 0;
    std::wstring dllPath;
    std::wstring exePath;
    std::vector<std::wstring> appArgs;
};

struct Handle
{
    HANDLE value = nullptr;

    Handle() = default;
    explicit Handle(HANDLE handle) : value(handle) {}
    ~Handle()
    {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    Handle(Handle&& other) noexcept : value(other.value)
    {
        other.value = nullptr;
    }

    Handle& operator=(Handle&& other) noexcept
    {
        if (this != &other)
        {
            if (value && value != INVALID_HANDLE_VALUE)
                CloseHandle(value);
            value = other.value;
            other.value = nullptr;
        }
        return *this;
    }
};

std::wstring getLastErrorMessage(DWORD error = GetLastError())
{
    wchar_t* buffer = nullptr;
    FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        0,
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr);

    std::wstring message = buffer ? buffer : L"unknown error";
    if (buffer)
        LocalFree(buffer);

    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' || message.back() == L'.'))
        message.pop_back();
    return message;
}

std::wstring quoteArg(const std::wstring& value)
{
    if (value.empty())
        return L"\"\"";

    const bool needsQuotes = value.find_first_of(L" \t\"") != std::wstring::npos;
    if (!needsQuotes)
        return value;

    std::wstring quoted = L"\"";
    size_t backslashes = 0;
    for (wchar_t ch : value)
    {
        if (ch == L'\\')
        {
            ++backslashes;
        }
        else if (ch == L'"')
        {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(ch);
            backslashes = 0;
        }
        else
        {
            quoted.append(backslashes, L'\\');
            backslashes = 0;
            quoted.push_back(ch);
        }
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

std::wstring buildCommandLine(const std::wstring& exePath, const std::vector<std::wstring>& args)
{
    std::wstring commandLine = quoteArg(exePath);
    for (const auto& arg : args)
    {
        commandLine.push_back(L' ');
        commandLine += quoteArg(arg);
    }
    return commandLine;
}

void printUsage()
{
    std::wcout
        << L"fsr2injector\n\n"
        << L"Usage:\n"
        << L"  fsr2injector --list\n"
        << L"  fsr2injector --dll <bridge.dll> --pid <pid> [--dry-run]\n"
        << L"  fsr2injector --dll <bridge.dll> --exe <app.exe> [--wait] [-- app args]\n\n"
        << L"Notes:\n"
        << L"  The injected DLL must match the target process architecture.\n"
        << L"  Avoid protected, anti-cheat, DRM, or multiplayer processes.\n";
}

std::optional<DWORD> parseDword(const std::wstring& value)
{
    wchar_t* end = nullptr;
    const unsigned long parsed = wcstoul(value.c_str(), &end, 10);
    if (!end || *end != L'\0' || parsed == 0 || parsed > MAXDWORD)
        return std::nullopt;
    return static_cast<DWORD>(parsed);
}

std::optional<Options> parseArgs(int argc, wchar_t** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i)
    {
        const std::wstring arg = argv[i];
        if (arg == L"--help" || arg == L"-h")
        {
            printUsage();
            return std::nullopt;
        }
        if (arg == L"--list")
        {
            options.list = true;
        }
        else if (arg == L"--dry-run")
        {
            options.dryRun = true;
        }
        else if (arg == L"--wait")
        {
            options.waitForExit = true;
        }
        else if (arg == L"--dll" && i + 1 < argc)
        {
            options.dllPath = argv[++i];
        }
        else if (arg == L"--pid" && i + 1 < argc)
        {
            auto pid = parseDword(argv[++i]);
            if (!pid)
            {
                std::wcerr << L"Invalid pid.\n";
                return std::nullopt;
            }
            options.pid = *pid;
        }
        else if (arg == L"--exe" && i + 1 < argc)
        {
            options.exePath = argv[++i];
        }
        else if (arg == L"--")
        {
            for (++i; i < argc; ++i)
                options.appArgs.emplace_back(argv[i]);
            break;
        }
        else
        {
            std::wcerr << L"Unknown argument: " << arg << L"\n";
            return std::nullopt;
        }
    }

    return options;
}

bool enableDebugPrivilege()
{
    Handle token;
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &rawToken))
        return false;
    token = Handle(rawToken);

    TOKEN_PRIVILEGES privileges = {};
    if (!LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &privileges.Privileges[0].Luid))
        return false;

    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    AdjustTokenPrivileges(token.value, FALSE, &privileges, sizeof(privileges), nullptr, nullptr);
    return GetLastError() == ERROR_SUCCESS;
}

std::wstring machineName(USHORT machine)
{
    switch (machine)
    {
    case IMAGE_FILE_MACHINE_UNKNOWN: return L"native";
    case IMAGE_FILE_MACHINE_I386: return L"x86";
    case IMAGE_FILE_MACHINE_AMD64: return L"x64";
    case IMAGE_FILE_MACHINE_ARM64: return L"arm64";
    default: return L"machine-" + std::to_wstring(machine);
    }
}

bool processMachine(HANDLE process, USHORT& processMachineValue, USHORT& nativeMachineValue)
{
    using IsWow64Process2Fn = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
    auto fn = reinterpret_cast<IsWow64Process2Fn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2"));
    if (fn)
        return fn(process, &processMachineValue, &nativeMachineValue) != FALSE;

    BOOL isWow64 = FALSE;
    if (!IsWow64Process(process, &isWow64))
        return false;

#if defined(_M_X64)
    nativeMachineValue = IMAGE_FILE_MACHINE_AMD64;
    processMachineValue = isWow64 ? IMAGE_FILE_MACHINE_I386 : IMAGE_FILE_MACHINE_UNKNOWN;
#elif defined(_M_IX86)
    nativeMachineValue = IMAGE_FILE_MACHINE_I386;
    processMachineValue = IMAGE_FILE_MACHINE_UNKNOWN;
#else
    nativeMachineValue = IMAGE_FILE_MACHINE_UNKNOWN;
    processMachineValue = IMAGE_FILE_MACHINE_UNKNOWN;
#endif
    return true;
}

bool architectureMatches(HANDLE target)
{
    USHORT selfMachine = IMAGE_FILE_MACHINE_UNKNOWN;
    USHORT selfNative = IMAGE_FILE_MACHINE_UNKNOWN;
    USHORT targetMachine = IMAGE_FILE_MACHINE_UNKNOWN;
    USHORT targetNative = IMAGE_FILE_MACHINE_UNKNOWN;

    if (!processMachine(GetCurrentProcess(), selfMachine, selfNative) ||
        !processMachine(target, targetMachine, targetNative))
    {
        std::wcerr << L"Warning: could not verify process architecture: " << getLastErrorMessage() << L"\n";
        return true;
    }

    if (selfMachine != targetMachine || selfNative != targetNative)
    {
        std::wcerr
            << L"Architecture mismatch. Injector is " << machineName(selfMachine)
            << L" on " << machineName(selfNative)
            << L", target is " << machineName(targetMachine)
            << L" on " << machineName(targetNative) << L".\n";
        return false;
    }

    return true;
}

bool injectDll(DWORD pid, const std::filesystem::path& dllPath, bool dryRun)
{
    enableDebugPrivilege();

    Handle process(OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE,
        pid));
    if (!process.value)
    {
        std::wcerr << L"OpenProcess failed for pid " << pid << L": " << getLastErrorMessage() << L"\n";
        return false;
    }

    if (!architectureMatches(process.value))
        return false;

    const std::wstring dllString = std::filesystem::absolute(dllPath).wstring();
    const SIZE_T bytes = (dllString.size() + 1) * sizeof(wchar_t);

    std::wcout << L"Target pid: " << pid << L"\n";
    std::wcout << L"Bridge DLL: " << dllString << L"\n";

    if (dryRun)
    {
        std::wcout << L"Dry run complete; no DLL was injected.\n";
        return true;
    }

    LPVOID remotePath = VirtualAllocEx(process.value, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath)
    {
        std::wcerr << L"VirtualAllocEx failed: " << getLastErrorMessage() << L"\n";
        return false;
    }

    if (!WriteProcessMemory(process.value, remotePath, dllString.c_str(), bytes, nullptr))
    {
        std::wcerr << L"WriteProcessMemory failed: " << getLastErrorMessage() << L"\n";
        VirtualFreeEx(process.value, remotePath, 0, MEM_RELEASE);
        return false;
    }

    auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    if (!loadLibrary)
    {
        std::wcerr << L"Could not resolve LoadLibraryW.\n";
        VirtualFreeEx(process.value, remotePath, 0, MEM_RELEASE);
        return false;
    }

    Handle thread(CreateRemoteThread(process.value, nullptr, 0, loadLibrary, remotePath, 0, nullptr));
    if (!thread.value)
    {
        std::wcerr << L"CreateRemoteThread failed: " << getLastErrorMessage() << L"\n";
        VirtualFreeEx(process.value, remotePath, 0, MEM_RELEASE);
        return false;
    }

    WaitForSingleObject(thread.value, INFINITE);

    DWORD remoteResult = 0;
    GetExitCodeThread(thread.value, &remoteResult);
    VirtualFreeEx(process.value, remotePath, 0, MEM_RELEASE);

    if (remoteResult == 0)
    {
        std::wcerr << L"LoadLibraryW failed in the target process.\n";
        return false;
    }

    std::wcout << L"Injection complete.\n";
    return true;
}

bool launchAndInject(const Options& options, DWORD& launchedPid, Handle& launchedProcess)
{
    std::wstring commandLine = buildCommandLine(options.exePath, options.appArgs);
    STARTUPINFOW startup = {};
    PROCESS_INFORMATION processInfo = {};
    startup.cb = sizeof(startup);

    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');

    const std::filesystem::path exePath(options.exePath);
    const std::wstring workingDirectory = exePath.has_parent_path() ? exePath.parent_path().wstring() : L"";

    if (!CreateProcessW(
            options.exePath.c_str(),
            mutableCommandLine.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_SUSPENDED,
            nullptr,
            workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
            &startup,
            &processInfo))
    {
        std::wcerr << L"CreateProcessW failed: " << getLastErrorMessage() << L"\n";
        return false;
    }

    Handle thread(processInfo.hThread);
    launchedProcess = Handle(processInfo.hProcess);
    launchedPid = processInfo.dwProcessId;

    if (!injectDll(launchedPid, options.dllPath, options.dryRun))
    {
        TerminateProcess(launchedProcess.value, 1);
        return false;
    }

    ResumeThread(thread.value);
    return true;
}

void listProcesses()
{
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snapshot.value == INVALID_HANDLE_VALUE)
    {
        std::wcerr << L"CreateToolhelp32Snapshot failed: " << getLastErrorMessage() << L"\n";
        return;
    }

    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot.value, &entry))
    {
        std::wcerr << L"Process32FirstW failed: " << getLastErrorMessage() << L"\n";
        return;
    }

    std::wcout << L"PID\tName\n";
    do
    {
        std::wcout << entry.th32ProcessID << L"\t" << entry.szExeFile << L"\n";
    } while (Process32NextW(snapshot.value, &entry));
}
}

int wmain(int argc, wchar_t** argv)
{
    auto parsed = parseArgs(argc, argv);
    if (!parsed)
        return 2;

    Options options = *parsed;
    if (options.list)
    {
        listProcesses();
        return 0;
    }

    const bool attachMode = options.pid != 0;
    const bool launchMode = !options.exePath.empty();
    if (attachMode == launchMode || options.dllPath.empty())
    {
        printUsage();
        return 2;
    }

    if (!std::filesystem::exists(options.dllPath))
    {
        std::wcerr << L"Bridge DLL does not exist: " << options.dllPath << L"\n";
        return 2;
    }

    if (launchMode && !std::filesystem::exists(options.exePath))
    {
        std::wcerr << L"Executable does not exist: " << options.exePath << L"\n";
        return 2;
    }

    if (attachMode)
        return injectDll(options.pid, options.dllPath, options.dryRun) ? 0 : 1;

    DWORD launchedPid = 0;
    Handle launchedProcess;
    if (!launchAndInject(options, launchedPid, launchedProcess))
        return 1;

    std::wcout << L"Launched pid: " << launchedPid << L"\n";
    if (options.waitForExit)
    {
        WaitForSingleObject(launchedProcess.value, INFINITE);
        DWORD exitCode = 0;
        GetExitCodeProcess(launchedProcess.value, &exitCode);
        std::wcout << L"Target exited with code " << exitCode << L".\n";
        return static_cast<int>(exitCode);
    }

    return 0;
}
