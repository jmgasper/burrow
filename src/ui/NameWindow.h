// Asks for a profile name, then sends the template message with "name" added.
#pragma once
#include <Messenger.h>
#include <Window.h>

class BButton;
class BTextControl;

namespace burrow {

class NameWindow : public BWindow {
public:
    NameWindow(const char* title, const char* label, const char* name, const char* action,
        const BMessage& message, BMessenger target);
    void MessageReceived(BMessage* message) override;

private:
    BTextControl* fName;
    BButton* fOK;
    BMessage fMessage;
    BMessenger fTarget;
};

}  // namespace burrow
