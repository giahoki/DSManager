#pragma once
// Interface language. Every UI string is written in place in both languages: tr("Применить", "Apply").
#include <string>

enum class Lang { Ru, En };
enum class LangPref { Auto, Ru, En };  // what the user picked; Auto follows the Windows display language

Lang systemLang();
Lang lang();
void setLang(Lang l);
const char* langCode();  // "ru" / "en": also the name of a mod's translation file, lang/<code>.json

inline const char* tr(const char* ru, const char* en) { return lang() == Lang::Ru ? ru : en; }

// A string of a data table (game names, quotes) in both languages.
struct Text {
    const char* ru = "";
    const char* en = "";
    const char* get() const { return tr(ru, en); }
    bool empty() const { return !*ru && !*en; }
};
