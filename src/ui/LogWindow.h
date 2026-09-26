// A profile's connection log: OpenVPN's messages and Burrow's own notes.
#pragma once
#include <Messenger.h>
#include <Window.h>
#include <string>
#include <vector>

class BTextView;

namespace burrow {

class LogWindow : public BWindow {
public:
    LogWindow(const std::string& profileName, const std::vector<std::string>& lines,
        const BMessage& closed, BMessenger target);
    void MessageReceived(BMessage* message) override;
    bool QuitRequested() override;

private:
    void _Append(const std::string& line);

    BTextView* fText;
    BMessage fClosed;
    BMessenger fTarget;
};

}  // namespace burrow
