// Imported profiles, kept as private copies in Burrow's settings directory:
// <id>.ovpn holds the configuration, <id>.name the display name.
#pragma once
#include "Profile.h"
#include <string>
#include <vector>

namespace burrow {

struct StoredProfile {
    std::string id;
    std::string name;
    std::string text;
    ProfileConfig config;
};

class ProfileStore {
public:
    explicit ProfileStore(const std::string& directory);

    bool Load();
    const std::vector<StoredProfile>& Profiles() const { return fProfiles; }
    const StoredProfile* Find(const std::string& id) const;

    // Checks that text looks like an OpenVPN client profile.
    static bool Validate(const std::string& text, std::string& error);

    bool Import(const std::string& text, const std::string& name, std::string& id,
        std::string& error);
    bool Rename(const std::string& id, const std::string& name);
    bool Remove(const std::string& id);
    std::string UniqueName(const std::string& name) const;

private:
    void _Sort();

    std::string fDirectory;
    std::vector<StoredProfile> fProfiles;
};

}  // namespace burrow
