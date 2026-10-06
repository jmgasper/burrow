// Exercise dialog layout using long names/challenges, without opening a VPN.
#include <Application.h>
#include <Button.h>
#include <TextControl.h>
#include <TextView.h>
#include <cstdio>
#include <string>
#include "ui/CredentialsWindow.h"
#include "ui/NameWindow.h"
#include "ui/LogWindow.h"
using namespace burrow;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
static void CheckView(BView* view)
{
    if (view->IsHidden()) return;
    CHECK(view->Frame().IsValid());
    for (int32 i = 0; BView* child = view->ChildAt(i); ++i) {
        if (child->IsHidden()) continue;
        if (dynamic_cast<BControl*>(child))
            CHECK(view->Bounds().InsetByCopy(-1, -1).Contains(child->Frame()));
        CheckView(child);
    }
}
static void CheckWindow(BWindow* window)
{
    window->Show(); snooze(100000); window->Lock();
    for (int32 i = 0; BView* view = window->ChildAt(i); ++i) CheckView(view);
    CHECK(window->Frame().Width() < 1000);
}
int main()
{
    BApplication app("application/x-vnd.Burrow-UITests");
    const BFont plain(*be_plain_font), bold(*be_bold_font);
    for (float size : {12.0f, 18.0f}) {
        const_cast<BFont*>(be_plain_font)->SetSize(size);
        const_cast<BFont*>(be_bold_font)->SetSize(size);
        std::string name = "Engineering / development / regional VPN profile with a long descriptive name";
        std::string prompt = "Enter the six digit verification code from your authenticator. "
            "If you cannot access your authenticator, use a recovery code provided by your administrator.";
        for (bool retry : {false, true}) {
            auto* credentials = new CredentialsWindow("ui-test", name, prompt, false, retry, BMessenger());
            CheckWindow(credentials);
            auto* challenge = dynamic_cast<BTextView*>(credentials->FindView("challenge"));
            CHECK(challenge && challenge->CountLines() > 1);
            credentials->Quit();
        }
        auto* emptyName = new NameWindow("Name", "Name:", "   ", "Import", BMessage(), BMessenger());
        CheckWindow(emptyName);
        CHECK(emptyName->DefaultButton() && !emptyName->DefaultButton()->IsEnabled());
        emptyName->Quit();
        auto* log = new LogWindow("UI test", {"A sample log line"}, BMessage(), BMessenger());
        CheckWindow(log); log->Quit();
    }
    *const_cast<BFont*>(be_plain_font) = plain;
    *const_cast<BFont*>(be_bold_font) = bold;
    std::printf("UI tests: %d failures\n", failures);
    return failures ? 1 : 0;
}
