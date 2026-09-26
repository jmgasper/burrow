// Asks for a user name and password, or for the response to a server challenge.
#pragma once
#include <Messenger.h>
#include <Window.h>
#include <string>

class BTextControl;

namespace burrow {

class CredentialsWindow : public BWindow {
public:
    CredentialsWindow(const std::string& id, const std::string& profileName,
        const std::string& challenge, bool echo, bool retry, BMessenger target);
    void MessageReceived(BMessage* message) override;
    bool QuitRequested() override;

private:
    std::string fId;
    BTextControl* fUser = nullptr;
    BTextControl* fPassword = nullptr;
    BTextControl* fResponse = nullptr;
    BMessenger fTarget;
    bool fAnswered = false;
};

}  // namespace burrow
