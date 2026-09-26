#include "ProfileStore.h"
#include "ResolvConf.h"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <random>
#include <sys/stat.h>
#include <unistd.h>

namespace burrow {

namespace {

constexpr size_t kMaxProfileSize = 1024 * 1024;

std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
        [](unsigned char c) { return std::tolower(c); });
    return text;
}

std::string Trim(const std::string& text)
{
    size_t start = text.find_first_not_of(" \t\r\n");
    if (start == std::string::npos)
        return "";
    return text.substr(start, text.find_last_not_of(" \t\r\n") - start + 1);
}

bool MakeDirectories(const std::string& path)
{
    std::string partial;
    size_t position = 0;
    while (position != std::string::npos) {
        position = path.find('/', position + 1);
        partial = path.substr(0, position);
        if (!partial.empty() && mkdir(partial.c_str(), 0700) != 0 && errno != EEXIST)
            return false;
    }
    return true;
}

}  // namespace


ProfileStore::ProfileStore(const std::string& directory)
    :
    fDirectory(directory)
{
}


bool ProfileStore::Load()
{
    fProfiles.clear();
    DIR* dir = opendir(fDirectory.c_str());
    if (dir == nullptr)
        return errno == ENOENT;
    while (dirent* entry = readdir(dir)) {
        std::string file = entry->d_name;
        if (file.size() <= 5 || file.compare(file.size() - 5, 5, ".ovpn") != 0)
            continue;
        StoredProfile profile;
        profile.id = file.substr(0, file.size() - 5);
        if (!ReadFile(fDirectory + "/" + file, profile.text))
            continue;
        ReadFile(fDirectory + "/" + profile.id + ".name", profile.name);
        profile.name = Trim(profile.name);
        profile.config = ParseProfile(profile.text);
        if (profile.name.empty())
            profile.name = SuggestProfileName(profile.config, file);
        fProfiles.push_back(profile);
    }
    closedir(dir);
    _Sort();
    return true;
}


const StoredProfile* ProfileStore::Find(const std::string& id) const
{
    for (const StoredProfile& profile : fProfiles) {
        if (profile.id == id)
            return &profile;
    }
    return nullptr;
}


bool ProfileStore::Validate(const std::string& text, std::string& error)
{
    if (text.size() > kMaxProfileSize) {
        error = "The file is too large to be a VPN profile.";
        return false;
    }
    if (text.find('\0') != std::string::npos) {
        error = "The file is not a text file.";
        return false;
    }
    ProfileConfig config = ParseProfile(text);
    if (config.remotes.empty()) {
        error = "The file names no VPN server (no \"remote\" line). "
            "Is it an OpenVPN or AWS Client VPN profile?";
        return false;
    }
    return true;
}


bool ProfileStore::Import(const std::string& text, const std::string& name, std::string& id,
    std::string& error)
{
    if (!Validate(text, error))
        return false;
    if (!MakeDirectories(fDirectory)) {
        error = "Cannot create " + fDirectory + ": " + strerror(errno);
        return false;
    }
    std::random_device device;
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%08lx%04x", (unsigned long)time(nullptr),
        (unsigned)(device() & 0xffff));
    id = buffer;
    // Profiles often embed private keys: owner-only access.
    if (!WriteFileAtomic(fDirectory + "/" + id + ".ovpn", text, 0600)
        || !WriteFileAtomic(fDirectory + "/" + id + ".name", UniqueName(name) + "\n", 0600)) {
        error = "Cannot save the profile: " + std::string(strerror(errno));
        return false;
    }
    return Load();
}


bool ProfileStore::Rename(const std::string& id, const std::string& name)
{
    if (Find(id) == nullptr || Trim(name).empty())
        return false;
    if (!WriteFileAtomic(fDirectory + "/" + id + ".name", Trim(name) + "\n", 0600))
        return false;
    return Load();
}


bool ProfileStore::Remove(const std::string& id)
{
    if (Find(id) == nullptr)
        return false;
    unlink((fDirectory + "/" + id + ".ovpn").c_str());
    unlink((fDirectory + "/" + id + ".name").c_str());
    return Load();
}


std::string ProfileStore::UniqueName(const std::string& name) const
{
    std::string base = Trim(name).empty() ? "VPN" : Trim(name);
    std::string candidate = base;
    for (int i = 2; ; i++) {
        bool taken = false;
        for (const StoredProfile& profile : fProfiles)
            taken |= Lower(profile.name) == Lower(candidate);
        if (!taken)
            return candidate;
        candidate = base + " " + std::to_string(i);
    }
}


void ProfileStore::_Sort()
{
    std::sort(fProfiles.begin(), fProfiles.end(), [](const StoredProfile& a,
            const StoredProfile& b) {
        return Lower(a.name) < Lower(b.name);
    });
}

}  // namespace burrow
