#pragma once
#include "i18n.h"
#include "render.h"
#include "util.h"
#include <string>
#include <vector>

// Colours of the whole UI plus the background, per game.
struct Theme {
    Col parch, parchDim, ash;  // text
    Col gold, goldDim;         // frames and ornaments
    Col ember, emberHot;       // the game's light: selection, lit mods, glows
    Col blood;                 // warnings
    Backdrop bg;
};
Theme mix(const Theme& a, const Theme& b, float t);
const Theme& selectionTheme();

// One supported game. Adding a game = adding an entry in games().
struct GameDef {
    std::string id;          // folder name under data/
    std::string name;        // shown in UI and used as mods/<name>
    std::wstring exe;        // main executable, used to verify the folder and to detect a running game
    std::wstring steamDir;   // default folder name under steamapps/common
    std::wstring subDir;     // where the exe lives inside it ("Game" for DS3 / Elden Ring); mods mirror this folder
    std::string steamAppId;
    Text realm;              // "Лордран · 2018"
    Text tagline;            // one quiet line on the selection card
    Text warning;            // anti-cheat / loader note, empty if none
    Theme theme;
    std::string logo;        // RCDATA name in src/DSM.rc (from assets/icons), empty: draw the generic sigil
    BurnStyle burn;          // how its screen burns in when you enter
};

const std::vector<GameDef>& games();

// Finds the game through Steam (registry -> every library in libraryfolders.vdf -> appmanifest).
fs::path locateGame(const GameDef& g);
bool isGameFolder(const GameDef& g, const fs::path& dir);
