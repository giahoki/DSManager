#include "settings.h"

#include <json.hpp>

using json = nlohmann::json;

static fs::path file(const fs::path& appRoot) { return appRoot / "data" / "settings.json"; }

void AppSettings::load(const fs::path& appRoot)
{
    json j = json::parse(readTextFile(file(appRoot)), nullptr, false);
    if (!j.is_object()) return;
    std::string l = j.value("language", "auto");
    language = l == "ru" ? LangPref::Ru : l == "en" ? LangPref::En : LangPref::Auto;
    debug = j.value("debug", false);
}

void AppSettings::save(const fs::path& appRoot) const
{
    json j;
    j["language"] = language == LangPref::Ru ? "ru" : language == LangPref::En ? "en" : "auto";
    j["debug"] = debug;
    std::error_code ec;
    fs::create_directories(file(appRoot).parent_path(), ec);
    writeTextFile(file(appRoot), j.dump(2));
}

Lang AppSettings::effectiveLang() const
{
    return language == LangPref::Ru ? Lang::Ru : language == LangPref::En ? Lang::En : systemLang();
}
