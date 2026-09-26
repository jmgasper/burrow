#include "SamlListener.h"
#include "Protocol.h"
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace burrow {

namespace {

constexpr size_t kMaxRequest = 4 * 1024 * 1024;

const char kSignedInPage[] =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>Signed in</title>"
    "<style>body{font-family:sans-serif;background:#f4f6f8;color:#1d2733;margin:0}"
    "main{max-width:26em;margin:12vh auto;background:#fff;border-radius:10px;"
    "padding:2em 2.2em;box-shadow:0 2px 12px rgba(0,0,0,.08)}"
    "h1{font-size:1.3em;margin:0 0 .6em}p{line-height:1.45;color:#445}"
    ".ok{display:inline-block;width:1.4em;height:1.4em;border-radius:50%;background:#2e9d5b;"
    "color:#fff;text-align:center;line-height:1.4em;margin-right:.4em}</style></head>"
    "<body><main><h1><span class=\"ok\">&#10003;</span>You are signed in</h1>"
    "<p>Burrow received your sign-in and is finishing the VPN connection. "
    "You can close this window.</p></main></body></html>";

const char kWaitingPage[] =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>Burrow</title></head>"
    "<body style=\"font-family:sans-serif\"><p>Burrow is waiting for a single sign-on "
    "response. Start the connection from Burrow.</p></body></html>";

void SendResponse(int fd, const char* status, const char* body)
{
    std::string response = std::string("HTTP/1.1 ") + status + "\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n"
        "Content-Length: " + std::to_string(strlen(body)) + "\r\n\r\n" + body;
    size_t sent = 0;
    while (sent < response.size()) {
        ssize_t result = send(fd, response.data() + sent, response.size() - sent, 0);
        if (result <= 0)
            break;
        sent += result;
    }
}

std::string Lower(std::string text)
{
    for (char& c : text)
        c = tolower((unsigned char)c);
    return text;
}

}  // namespace


SamlListener::SamlListener(Callback callback, int port)
    :
    fCallback(std::move(callback)),
    fPort(port)
{
}


SamlListener::~SamlListener()
{
    Stop();
}


bool SamlListener::Start(std::string& error)
{
    if (Running())
        return true;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        error = strerror(errno);
        return false;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(fPort);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (sockaddr*)&address, sizeof(address)) != 0 || listen(fd, 8) != 0) {
        error = "Cannot listen on 127.0.0.1:" + std::to_string(fPort) + ": " + strerror(errno)
            + " (is another VPN client signing in?)";
        close(fd);
        return false;
    }
    if (pipe(fWakePipe) != 0) {
        error = strerror(errno);
        close(fd);
        return false;
    }
    fListenFd = fd;
    fThread = std::thread(&SamlListener::_Serve, this);
    return true;
}


void SamlListener::Stop()
{
    if (!Running())
        return;
    char byte = 0;
    if (write(fWakePipe[1], &byte, 1) < 0) {}
    if (fThread.joinable())
        fThread.join();
    close(fListenFd);
    close(fWakePipe[0]);
    close(fWakePipe[1]);
    fListenFd = -1;
    fWakePipe[0] = fWakePipe[1] = -1;
}


void SamlListener::_Serve()
{
    while (true) {
        pollfd fds[2] = {{fListenFd, POLLIN, 0}, {fWakePipe[0], POLLIN, 0}};
        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        if (fds[1].revents)
            return;
        if (fds[0].revents & POLLIN) {
            int client = accept(fListenFd, nullptr, nullptr);
            if (client >= 0) {
                _Handle(client);
                close(client);
            }
        }
    }
}


void SamlListener::_Handle(int fd)
{
    std::string request;
    size_t headerEnd = std::string::npos;
    size_t contentLength = 0;
    char buffer[16384];
    while (request.size() < kMaxRequest) {
        pollfd pfd = {fd, POLLIN, 0};
        if (poll(&pfd, 1, 15000) <= 0)
            return;
        ssize_t received = recv(fd, buffer, sizeof(buffer), 0);
        if (received <= 0)
            break;
        request.append(buffer, received);
        if (headerEnd == std::string::npos) {
            headerEnd = request.find("\r\n\r\n");
            if (headerEnd != std::string::npos) {
                std::string headers = Lower(request.substr(0, headerEnd));
                size_t length = headers.find("\ncontent-length:");
                if (length != std::string::npos)
                    contentLength = strtoul(headers.c_str() + length + 16, nullptr, 10);
            }
        }
        if (headerEnd != std::string::npos && request.size() >= headerEnd + 4 + contentLength)
            break;
    }
    if (headerEnd == std::string::npos)
        return;

    if (request.compare(0, 5, "POST ") != 0) {
        bool root = request.compare(0, 6, "GET / ") == 0;
        SendResponse(fd, root ? "200 OK" : "404 Not Found", kWaitingPage);
        return;
    }
    std::string body = request.substr(headerEnd + 4, contentLength);
    std::string saml = FormValue(body, "SAMLResponse");
    if (saml.empty()) {
        SendResponse(fd, "400 Bad Request", kWaitingPage);
        return;
    }
    SendResponse(fd, "200 OK", kSignedInPage);
    fCallback(saml);
}


std::string SamlListener::FormValue(const std::string& body, const std::string& name)
{
    size_t start = 0;
    while (start <= body.size()) {
        size_t end = body.find('&', start);
        if (end == std::string::npos)
            end = body.size();
        std::string pair = body.substr(start, end - start);
        size_t equals = pair.find('=');
        if (equals != std::string::npos && UrlDecode(pair.substr(0, equals)) == name)
            return UrlDecode(pair.substr(equals + 1));
        start = end + 1;
    }
    return "";
}

}  // namespace burrow
