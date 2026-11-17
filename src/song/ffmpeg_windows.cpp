// SPDX-License-Identifier: MIT

// FFmpeg's program on Windows (song/ffmpeg.h). The program reads its input from one socket on 127.0.0.1 and writes its
// output to another, each a URL in its arguments. Under Wine, the program can be a Linux program: Wine runs it, but
// gives it /dev/null in place of a pipe, and a process handle that never signals. So the sockets carry the bytes, and
// the end of the output's connection ends the work. A process handle that signals gives the program's exit code too.

// clang-format off
// Winsock's header comes before Windows' header.
#include <winsock2.h>
#include <windows.h>
// clang-format on

#include "song/ffmpeg.h"

#include <array>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>

namespace hokuto::song
{
namespace
{

// How long the program can take to connect, and to send or take the next bytes.
constexpr auto kConnectTime = std::chrono::seconds(30);
constexpr int kQuietSeconds = 60;

// The places of a Linux program through Wine's drive Z:, after PATH.
constexpr std::array<const wchar_t*, 3> kWinePlaces = {L"Z:\\usr\\bin\\ffmpeg", L"Z:\\usr\\local\\bin\\ffmpeg",
                                                       L"Z:\\snap\\bin\\ffmpeg"};

// The path of the ffmpeg program, or an empty string.
std::wstring FfmpegPath()
{
    std::array<wchar_t, MAX_PATH> found{};
    if (SearchPathW(nullptr, L"ffmpeg.exe", nullptr, static_cast<DWORD>(found.size()), found.data(), nullptr) > 0)
    {
        return found.data();
    }

    for (const wchar_t* place : kWinePlaces)
    {
        const DWORD attributes = GetFileAttributesW(place);
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        {
            return place;
        }
    }

    return {};
}

// Winsock, from the constructor to the destructor.
class Winsock
{
public:
    explicit Winsock(const char* doing)
    {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
        {
            throw std::runtime_error(FfmpegFailed(doing, "Windows sockets didn't start."));
        }
    }

    Winsock(const Winsock&) = delete;
    Winsock& operator=(const Winsock&) = delete;

    ~Winsock()
    {
        WSACleanup();
    }
};

// A socket that closes with the object.
class Socket
{
public:
    Socket() = default;
    explicit Socket(SOCKET socket) : socket_(socket)
    {
    }

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    Socket(Socket&& other) noexcept : socket_(other.socket_)
    {
        other.socket_ = INVALID_SOCKET;
    }

    ~Socket()
    {
        if (socket_ != INVALID_SOCKET)
        {
            closesocket(socket_);
        }
    }

    SOCKET Get() const
    {
        return socket_;
    }

private:
    SOCKET socket_ = INVALID_SOCKET;
};

// A socket that listens on 127.0.0.1, on a port that the system chooses, and its port.
Socket Listen(uint16_t& port, const char* doing)
{
    Socket listener(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int size = sizeof(address);
    if (listener.Get() == INVALID_SOCKET ||
        bind(listener.Get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(listener.Get(), 1) != 0 ||
        getsockname(listener.Get(), reinterpret_cast<sockaddr*>(&address), &size) != 0)
    {
        throw std::runtime_error(FfmpegFailed(doing, "Hokuto couldn't open a local socket for it."));
    }

    port = ntohs(address.sin_port);
    return listener;
}

// Whether `process` has ended. Wine's handle of a Linux program never signals, and the program then counts as running.
bool Ended(HANDLE process)
{
    return WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
}

// The connection that the program makes to `listener`, or an invalid socket when the program ends or takes too long.
SOCKET Accept(const Socket& listener, HANDLE process)
{
    const auto until = std::chrono::steady_clock::now() + kConnectTime;
    while (std::chrono::steady_clock::now() < until && !Ended(process))
    {
        fd_set ready;
        FD_ZERO(&ready);
        FD_SET(listener.Get(), &ready);
        const timeval wait{0, 100'000};
        if (select(0, &ready, nullptr, nullptr, &wait) > 0)
        {
            return accept(listener.Get(), nullptr, nullptr);
        }
    }

    return INVALID_SOCKET;
}

// Whether `socket` is ready to read, or to write, within kQuietSeconds.
bool Ready(SOCKET socket, bool write)
{
    fd_set ready;
    FD_ZERO(&ready);
    FD_SET(socket, &ready);
    const timeval wait{kQuietSeconds, 0};
    return select(0, write ? nullptr : &ready, write ? &ready : nullptr, nullptr, &wait) > 0;
}

// The arguments as one command line. Only the program's path can hold spaces: Hokuto's options hold none.
std::wstring CommandLine(const std::wstring& path, const std::vector<std::string>& arguments)
{
    std::wstring line = L"\"" + path + L"\"";
    for (const std::string& argument : arguments)
    {
        line += L" " + std::wstring(argument.begin(), argument.end());
    }

    return line;
}

} // namespace

bool FindsFfmpeg()
{
    return !FfmpegPath().empty();
}

std::vector<uint8_t> RunFfmpeg(const std::vector<std::string>& input_options, std::span<const uint8_t> input,
                               const std::vector<std::string>& output_options, const char* doing)
{
    const std::wstring path = FfmpegPath();
    if (path.empty())
    {
        throw std::runtime_error(FfmpegMissing(doing));
    }

    const Winsock winsock(doing);
    uint16_t in_port = 0;
    uint16_t out_port = 0;
    const Socket in_listener = Listen(in_port, doing);
    const Socket out_listener = Listen(out_port, doing);

    std::vector<std::string> arguments = {"-hide_banner", "-loglevel", "error", "-nostdin"};
    arguments.insert(arguments.end(), input_options.begin(), input_options.end());
    arguments.insert(arguments.end(), {"-i", "tcp://127.0.0.1:" + std::to_string(in_port)});
    arguments.insert(arguments.end(), output_options.begin(), output_options.end());
    arguments.push_back("tcp://127.0.0.1:" + std::to_string(out_port));
    std::wstring line = CommandLine(path, arguments);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(path.c_str(), line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                        &startup, &process))
    {
        throw std::runtime_error(FfmpegFailed(doing, "Windows couldn't start it."));
    }

    CloseHandle(process.hThread);

    // The input goes in on a thread of its own, while the output comes back here.
    bool sent = false;
    std::thread sender(
        [&]
        {
            const Socket connection(Accept(in_listener, process.hProcess));
            std::size_t at = 0;
            while (connection.Get() != INVALID_SOCKET && at < input.size() && Ready(connection.Get(), true))
            {
                const int length = static_cast<int>(std::min<std::size_t>(input.size() - at, 1 << 16));
                const int count = send(connection.Get(), reinterpret_cast<const char*>(input.data() + at), length, 0);
                if (count <= 0)
                {
                    break;
                }
                at += static_cast<std::size_t>(count);
            }

            sent = connection.Get() != INVALID_SOCKET && at == input.size();
            if (connection.Get() != INVALID_SOCKET)
            {
                shutdown(connection.Get(), SD_SEND);
            }
        });

    std::vector<uint8_t> output;
    bool ended = false;
    {
        const Socket connection(Accept(out_listener, process.hProcess));
        std::array<char, 1 << 16> buffer{};
        while (connection.Get() != INVALID_SOCKET && Ready(connection.Get(), false))
        {
            const int count = recv(connection.Get(), buffer.data(), static_cast<int>(buffer.size()), 0);
            if (count <= 0)
            {
                ended = count == 0;
                break;
            }
            output.insert(output.end(), buffer.data(), buffer.data() + count);
        }
    }

    sender.join();

    // A handle that signals gives the exit code. Wine's handle of a Linux program gives none.
    DWORD code = 0;
    const bool exited =
        WaitForSingleObject(process.hProcess, 10'000) == WAIT_OBJECT_0 && GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    if (!sent || !ended || output.empty())
    {
        throw std::runtime_error(FfmpegFailed(doing, "It stopped before it finished."));
    }
    if (exited && code != 0)
    {
        throw std::runtime_error(FfmpegFailed(doing, "It ended with code " + std::to_string(code) + "."));
    }

    return output;
}

} // namespace hokuto::song
