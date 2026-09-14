// Copyright (c) 2026 The Quicksilver developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <gpu/detection_service.h>

#include <charconv>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#ifdef WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace gpu {

std::optional<bool> GpuDetectionService::s_cached_has_gpu;
std::mutex GpuDetectionService::s_mutex;

namespace detail {
bool HasGpuFromNvidiaSmiOutput(std::string_view output)
{
    while (!output.empty()) {
        const size_t line_end{output.find_first_of("\r\n")};
        std::string_view line{output.substr(0, line_end)};
        const size_t first{line.find_first_not_of(" \t")};
        if (first != std::string_view::npos) {
            const size_t last{line.find_last_not_of(" \t")};
            line = line.substr(first, last - first + 1);
            int count{0};
            const auto [end, error]{std::from_chars(line.data(), line.data() + line.size(), count)};
            if (error == std::errc{} && end == line.data() + line.size() && count > 0) return true;
        }
        if (line_end == std::string_view::npos) break;
        const char line_break{output[line_end]};
        output.remove_prefix(line_end + 1);
        if (!output.empty() && output.front() == '\n' && line_break == '\r') output.remove_prefix(1);
    }
    return false;
}
} // namespace detail

#ifdef WIN32
namespace {
struct HandleCloser {
    void operator()(void* handle) const
    {
        if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;
} // namespace
#endif

bool GpuDetectionService::hasNvidiaDevice()
{
    constexpr auto TIMEOUT{std::chrono::seconds{3}};

#ifdef WIN32
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE read_raw{nullptr};
    HANDLE write_raw{nullptr};
    if (!CreatePipe(&read_raw, &write_raw, &security, 0)) return false;
    UniqueHandle read_pipe{read_raw};
    UniqueHandle write_pipe{write_raw};
    if (!SetHandleInformation(read_pipe.get(), HANDLE_FLAG_INHERIT, 0)) return false;

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write_pipe.get();
    startup.hStdError = write_pipe.get();
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION process{};
    std::wstring command{L"nvidia-smi.exe --query-gpu=count --format=csv,noheader"};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        return false;
    }
    UniqueHandle process_handle{process.hProcess};
    UniqueHandle thread_handle{process.hThread};
    write_pipe.reset();

    const DWORD wait_result{WaitForSingleObject(process_handle.get(), static_cast<DWORD>(TIMEOUT.count() * 1000))};
    if (wait_result != WAIT_OBJECT_0) {
        TerminateProcess(process_handle.get(), 1);
        WaitForSingleObject(process_handle.get(), INFINITE);
        return false;
    }

    DWORD exit_code{1};
    if (!GetExitCodeProcess(process_handle.get(), &exit_code) || exit_code != 0) return false;

    std::string output;
    char buffer[256];
    DWORD bytes_read{0};
    while (ReadFile(read_pipe.get(), buffer, sizeof(buffer), &bytes_read, nullptr) && bytes_read > 0) {
        output.append(buffer, bytes_read);
    }
    return detail::HasGpuFromNvidiaSmiOutput(output);
#else
    int output_pipe[2];
    if (pipe(output_pipe) != 0) return false;

    const pid_t child{fork()};
    if (child < 0) {
        close(output_pipe[0]);
        close(output_pipe[1]);
        return false;
    }

    if (child == 0) {
        close(output_pipe[0]);
        if (dup2(output_pipe[1], STDOUT_FILENO) < 0) _exit(127);
        close(output_pipe[1]);
        execlp("nvidia-smi", "nvidia-smi", "--query-gpu=count", "--format=csv,noheader", static_cast<char*>(nullptr));
        _exit(127);
    }

    close(output_pipe[1]);
    const auto deadline{std::chrono::steady_clock::now() + TIMEOUT};
    int status{0};
    while (true) {
        const pid_t result{waitpid(child, &status, WNOHANG)};
        if (result == child) break;
        if (result < 0 && errno != EINTR) {
            close(output_pipe[0]);
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            kill(child, SIGKILL);
            while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {
            }
            close(output_pipe[0]);
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }

    std::string output;
    char buffer[256];
    while (true) {
        const ssize_t bytes_read{read(output_pipe[0], buffer, sizeof(buffer))};
        if (bytes_read > 0) {
            output.append(buffer, static_cast<size_t>(bytes_read));
            continue;
        }
        if (bytes_read < 0 && errno == EINTR) continue;
        break;
    }
    close(output_pipe[0]);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 && detail::HasGpuFromNvidiaSmiOutput(output);
#endif
}

GpuState GpuDetectionService::detectGpu(bool is_configured)
{
    std::lock_guard<std::mutex> lock{s_mutex};
    if (!s_cached_has_gpu.has_value()) s_cached_has_gpu = hasNvidiaDevice();
    return GpuState{*s_cached_has_gpu, is_configured};
}

} // namespace gpu
