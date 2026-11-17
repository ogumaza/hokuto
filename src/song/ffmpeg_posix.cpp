// SPDX-License-Identifier: MIT

// FFmpeg's program on Linux (song/ffmpeg.h). The program reads its input from its standard input and writes its output
// to its standard output. Its standard error gives the message of a failure. The input goes through a socket pair in
// place of a pipe, so that a program that ends early raises no SIGPIPE in Hokuto.

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#include "song/ffmpeg.h"

extern char** environ;

namespace hokuto::song
{
namespace
{

// The path of the ffmpeg program in the folders of PATH, or an empty string.
std::string FfmpegPath()
{
    const char* folders = std::getenv("PATH");
    std::string_view rest = folders != nullptr ? folders : "";
    while (!rest.empty())
    {
        const std::size_t colon = rest.find(':');
        const std::string_view folder = rest.substr(0, colon);
        rest = colon == std::string_view::npos ? std::string_view() : rest.substr(colon + 1);
        if (folder.empty())
        {
            continue;
        }

        std::string path(folder);
        path += "/ffmpeg";
        if (access(path.c_str(), X_OK) == 0)
        {
            return path;
        }
    }

    return {};
}

// A file descriptor that closes with the object.
class Descriptor
{
public:
    Descriptor() = default;
    explicit Descriptor(int fd) : fd_(fd)
    {
    }

    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;

    Descriptor& operator=(Descriptor&& other) noexcept
    {
        if (this != &other)
        {
            Close();
            fd_ = other.fd_;
            other.fd_ = -1;
        }

        return *this;
    }

    ~Descriptor()
    {
        Close();
    }

    int Get() const
    {
        return fd_;
    }

    void Close()
    {
        if (fd_ >= 0)
        {
            close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_ = -1;
};

// The two ends of a pipe, or of a socket pair, that close on exec.
void Pair(Descriptor& read_end, Descriptor& write_end, bool socket_pair, const char* doing)
{
    std::array<int, 2> fds{};
    const int made =
        socket_pair ? socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds.data()) : pipe2(fds.data(), O_CLOEXEC);
    if (made != 0)
    {
        throw std::runtime_error(FfmpegFailed(doing, "Hokuto couldn't make a pipe for it."));
    }

    read_end = Descriptor(fds[0]);
    write_end = Descriptor(fds[1]);
}

} // namespace

bool FindsFfmpeg()
{
    return !FfmpegPath().empty();
}

std::vector<uint8_t> RunFfmpeg(const std::vector<std::string>& input_options, std::span<const uint8_t> input,
                               const std::vector<std::string>& output_options, const char* doing)
{
    const std::string path = FfmpegPath();
    if (path.empty())
    {
        throw std::runtime_error(FfmpegMissing(doing));
    }

    std::vector<std::string> arguments = {"ffmpeg", "-hide_banner", "-loglevel", "error", "-nostdin"};
    arguments.insert(arguments.end(), input_options.begin(), input_options.end());
    arguments.insert(arguments.end(), {"-i", "pipe:0"});
    arguments.insert(arguments.end(), output_options.begin(), output_options.end());
    arguments.push_back("pipe:1");
    std::vector<char*> argv;
    for (std::string& argument : arguments)
    {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);

    // The child's ends of the socket pair and the pipes become its standard input, output and error.
    Descriptor in_child;
    Descriptor in_parent;
    Descriptor out_parent;
    Descriptor out_child;
    Descriptor error_parent;
    Descriptor error_child;
    Pair(in_child, in_parent, true, doing);
    Pair(out_parent, out_child, false, doing);
    Pair(error_parent, error_child, false, doing);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, in_child.Get(), STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, out_child.Get(), STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, error_child.Get(), STDERR_FILENO);
    pid_t pid = 0;
    const int spawned = posix_spawn(&pid, path.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (spawned != 0)
    {
        throw std::runtime_error(FfmpegFailed(doing, "Linux couldn't start it."));
    }

    in_child.Close();
    out_child.Close();
    error_child.Close();

    // The input goes in on a thread of its own, while the output and the messages come back here.
    std::thread sender(
        [&]
        {
            std::size_t at = 0;
            while (at < input.size())
            {
                const ssize_t count = send(in_parent.Get(), input.data() + at, input.size() - at, MSG_NOSIGNAL);
                if (count < 0 && errno == EINTR)
                {
                    continue;
                }
                if (count <= 0)
                {
                    break;
                }
                at += static_cast<std::size_t>(count);
            }

            in_parent.Close();
        });

    std::vector<uint8_t> output;
    std::string messages;
    std::array<pollfd, 2> polled = {pollfd{out_parent.Get(), POLLIN, 0}, pollfd{error_parent.Get(), POLLIN, 0}};
    std::array<uint8_t, 1 << 16> buffer{};
    while (polled[0].fd >= 0 || polled[1].fd >= 0)
    {
        if (poll(polled.data(), polled.size(), -1) < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            break;
        }

        for (std::size_t i = 0; i < polled.size(); i++)
        {
            if (polled[i].fd < 0 || polled[i].revents == 0)
            {
                continue;
            }

            const ssize_t count = read(polled[i].fd, buffer.data(), buffer.size());
            if (count < 0 && errno == EINTR)
            {
                continue;
            }
            if (count <= 0)
            {
                polled[i].fd = -1;
                continue;
            }
            if (i == 0)
            {
                output.insert(output.end(), buffer.data(), buffer.data() + count);
            }
            else
            {
                messages.append(reinterpret_cast<const char*>(buffer.data()), static_cast<std::size_t>(count));
            }
        }
    }

    sender.join();
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
    {
    }

    while (!messages.empty() && (messages.back() == '\n' || messages.back() == '\r'))
    {
        messages.pop_back();
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || output.empty())
    {
        throw std::runtime_error(FfmpegFailed(doing, messages));
    }

    return output;
}

} // namespace hokuto::song
