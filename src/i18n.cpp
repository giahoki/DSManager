#include "i18n.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>

static std::atomic<Lang> g_lang{Lang::En};

Lang systemLang()
{
    // Russian also for the languages whose speakers usually read it better than English
    switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
    case LANG_RUSSIAN:
    case LANG_UKRAINIAN:
    case LANG_BELARUSIAN:
    case LANG_KAZAK:
        return Lang::Ru;
    default:
        return Lang::En;
    }
}

Lang lang() { return g_lang.load(std::memory_order_relaxed); }
void setLang(Lang l) { g_lang = l; }
const char* langCode() { return lang() == Lang::Ru ? "ru" : "en"; }
