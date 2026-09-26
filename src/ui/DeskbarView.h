// Burrow's Deskbar tray icon. It lives in Deskbar's team (Deskbar loads the
// Burrow executable as an add-on), asks the application for the profiles'
// states every two seconds, and offers connect/disconnect in its menu.
#pragma once
#include <View.h>
#include <string>
#include <vector>

class BBitmap;
class BMessageRunner;

class BurrowDeskbarView : public BView {
public:
    explicit BurrowDeskbarView(BRect frame);
    explicit BurrowDeskbarView(BMessage* archive);
    ~BurrowDeskbarView() override;

    static BArchivable* Instantiate(BMessage* archive);
    status_t Archive(BMessage* archive, bool deep = true) const override;

    void AttachedToWindow() override;
    void DetachedFromWindow() override;
    void Draw(BRect updateRect) override;
    void MouseDown(BPoint where) override;
    void MessageReceived(BMessage* message) override;

    static const char* kName;

private:
    struct Entry {
        std::string id;
        std::string name;
        int32 state;
    };

    void _Init();
    void _Poll();
    void _ShowMenu(BPoint where);
    int32 _OverallState() const;

    BBitmap* fIcon = nullptr;
    BMessageRunner* fPoller = nullptr;
    std::vector<Entry> fEntries;
    bool fAppRunning = false;
};
