#pragma once
// DSM's own settings, data/settings.json next to the exe.
#include "i18n.h"
#include "util.h"

struct AppSettings {
    LangPref language = LangPref::Auto;
    bool debug = false;  // install journal in the app + mods write their debug logs (mod.json "debug")

    void load(const fs::path& appRoot);
    void save(const fs::path& appRoot) const;
    Lang effectiveLang() const;
};
