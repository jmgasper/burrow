// A tiny HTTP server on 127.0.0.1:35001 that receives the SAMLResponse the
// identity provider's page posts after single sign-on.
#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace burrow {

class SamlListener {
public:
    // Called on the listener thread with the (form-decoded) SAMLResponse.
    using Callback = std::function<void(const std::string& samlResponse)>;

    explicit SamlListener(Callback callback, int port = 35001);
    ~SamlListener();

    // Binds and starts serving; fills error (e.g. port already in use) on failure.
    bool Start(std::string& error);
    void Stop();
    bool Running() const { return fListenFd >= 0; }
    int Port() const { return fPort; }

    // Parses an application/x-www-form-urlencoded body.
    static std::string FormValue(const std::string& body, const std::string& name);

private:
    void _Serve();
    void _Handle(int fd);

    Callback fCallback;
    int fPort;
    int fListenFd = -1;
    int fWakePipe[2] = {-1, -1};
    std::thread fThread;
};

}  // namespace burrow
