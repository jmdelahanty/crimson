#include "windows_crash_dump.h"

#include "ui_path_config.h"

#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <dbghelp.h>
#include <windows.h>

namespace {

std::filesystem::path g_crash_dump_dir;
std::filesystem::path g_executable_path;

std::filesystem::path GetDefaultCrashDumpDir() {
    if (const char* explicit_dir = std::getenv("CRIMSON_CRASH_DUMP_DIR");
        explicit_dir && *explicit_dir != '\0') {
        return std::filesystem::path(explicit_dir);
    }

    if (const char* localappdata = std::getenv("LOCALAPPDATA");
        localappdata && *localappdata != '\0') {
        return std::filesystem::path(localappdata) / "Crimson" / "CrashDumps";
    }

    std::error_code ec;
    const std::filesystem::path temp_dir = std::filesystem::temp_directory_path(ec);
    if (!ec && !temp_dir.empty()) {
        return temp_dir / "Crimson" / "CrashDumps";
    }

    return std::filesystem::path("CrimsonCrashDumps");
}

std::wstring MakeCrashBasename(const SYSTEMTIME& local_time, DWORD pid, DWORD tid) {
    wchar_t buffer[128];
    swprintf(buffer,
             sizeof(buffer) / sizeof(buffer[0]),
             L"crimson-crash-%04d%02d%02d-%02d%02d%02d-pid%lu-tid%lu",
             static_cast<int>(local_time.wYear),
             static_cast<int>(local_time.wMonth),
             static_cast<int>(local_time.wDay),
             static_cast<int>(local_time.wHour),
             static_cast<int>(local_time.wMinute),
             static_cast<int>(local_time.wSecond),
             static_cast<unsigned long>(pid),
             static_cast<unsigned long>(tid));
    return std::wstring(buffer);
}

std::string PointerToString(const void* value) {
    std::ostringstream stream;
    stream << value;
    return stream.str();
}

void WriteCrashMetadata(const std::filesystem::path& metadata_path,
                        const std::filesystem::path& dump_path,
                        const SYSTEMTIME& local_time,
                        EXCEPTION_POINTERS* exception_pointers,
                        bool dump_written,
                        DWORD dump_error) {
    std::ofstream metadata_stream(metadata_path, std::ios::out | std::ios::trunc);
    if (!metadata_stream.is_open()) {
        return;
    }

    metadata_stream << "timestamp_local="
                    << local_time.wYear << "-"
                    << (local_time.wMonth < 10 ? "0" : "") << local_time.wMonth << "-"
                    << (local_time.wDay < 10 ? "0" : "") << local_time.wDay << " "
                    << (local_time.wHour < 10 ? "0" : "") << local_time.wHour << ":"
                    << (local_time.wMinute < 10 ? "0" : "") << local_time.wMinute << ":"
                    << (local_time.wSecond < 10 ? "0" : "") << local_time.wSecond << "\n";
    metadata_stream << "pid=" << GetCurrentProcessId() << "\n";
    metadata_stream << "thread_id=" << GetCurrentThreadId() << "\n";
    metadata_stream << "dump_written=" << (dump_written ? "true" : "false") << "\n";
    metadata_stream << "dump_write_error=" << dump_error << "\n";
    metadata_stream << "dump_path=" << dump_path.string() << "\n";
    metadata_stream << "executable_path=" << g_executable_path.string() << "\n";
    metadata_stream << "command_line=" << GetCommandLineA() << "\n";

    if (exception_pointers && exception_pointers->ExceptionRecord) {
        metadata_stream << "exception_code=0x"
                        << std::hex
                        << static_cast<unsigned long>(
                               exception_pointers->ExceptionRecord->ExceptionCode)
                        << std::dec << "\n";
        metadata_stream << "exception_flags="
                        << exception_pointers->ExceptionRecord->ExceptionFlags
                        << "\n";
        metadata_stream << "exception_address="
                        << PointerToString(exception_pointers->ExceptionRecord->ExceptionAddress)
                        << "\n";
    } else {
        metadata_stream << "exception_code=<unknown>\n";
        metadata_stream << "exception_address=<unknown>\n";
    }
}

LONG WINAPI CrimsonUnhandledExceptionFilter(EXCEPTION_POINTERS* exception_pointers) {
    const DWORD process_id = GetCurrentProcessId();
    const DWORD thread_id = GetCurrentThreadId();
    SYSTEMTIME local_time{};
    GetLocalTime(&local_time);

    const std::wstring basename = MakeCrashBasename(local_time, process_id, thread_id);
    const std::filesystem::path dump_path =
        g_crash_dump_dir / std::filesystem::path(basename + L".dmp");
    const std::filesystem::path metadata_path =
        g_crash_dump_dir / std::filesystem::path(basename + L".txt");

    bool dump_written = false;
    DWORD dump_error = ERROR_SUCCESS;

    HANDLE dump_file = CreateFileW(
        dump_path.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (dump_file == INVALID_HANDLE_VALUE) {
        dump_error = GetLastError();
    } else {
        MINIDUMP_EXCEPTION_INFORMATION exception_info{};
        exception_info.ThreadId = thread_id;
        exception_info.ExceptionPointers = exception_pointers;
        exception_info.ClientPointers = FALSE;

        const MINIDUMP_TYPE dump_type = static_cast<MINIDUMP_TYPE>(
            MiniDumpWithDataSegs |
            MiniDumpWithHandleData |
            MiniDumpWithThreadInfo |
            MiniDumpWithUnloadedModules |
            MiniDumpIgnoreInaccessibleMemory);

        dump_written = MiniDumpWriteDump(
            GetCurrentProcess(),
            process_id,
            dump_file,
            dump_type,
            exception_pointers ? &exception_info : nullptr,
            nullptr,
            nullptr) == TRUE;
        if (!dump_written) {
            dump_error = GetLastError();
        }
        CloseHandle(dump_file);
    }

    WriteCrashMetadata(
        metadata_path,
        dump_path,
        local_time,
        exception_pointers,
        dump_written,
        dump_error);

    std::cerr << "[CrashDump] "
              << (dump_written ? "Wrote minidump to " : "Failed to write minidump to ")
              << dump_path << std::endl;
    if (!dump_written) {
        std::cerr << "[CrashDump] MiniDumpWriteDump error code: "
                  << dump_error << std::endl;
    }
    std::cerr << "[CrashDump] Metadata sidecar: " << metadata_path << std::endl;

    return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

void InstallWindowsCrashHandler(const std::filesystem::path& argv0_path) {
    g_crash_dump_dir = GetDefaultCrashDumpDir();
    std::error_code ec;
    std::filesystem::create_directories(g_crash_dump_dir, ec);
    if (ec) {
        std::cerr << "[CrashDump] Failed to create crash dump directory "
                  << g_crash_dump_dir << ": " << ec.message() << std::endl;
    }

    if (auto executable_path = ResolveExecutablePath(argv0_path)) {
        g_executable_path = *executable_path;
    } else {
        g_executable_path = argv0_path;
    }

    SetUnhandledExceptionFilter(CrimsonUnhandledExceptionFilter);
    std::cerr << "[CrashDump] Unhandled exception filter active. Dump directory: "
              << g_crash_dump_dir << std::endl;
}

#else

void InstallWindowsCrashHandler(const std::filesystem::path&) {}

#endif
