#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

std::string toUtf8(const std::wstring& w);
std::wstring fromUtf8(const std::string& s);
std::string pathUtf8(const fs::path& p);
fs::path pathFromUtf8(const std::string& s);

// FNV-1a 64 of the file contents, 0 if the file can't be read
uint64_t hashFile(const fs::path& p);
std::string hashHex(uint64_t h);
uint64_t hashFromHex(const std::string& s);

bool isProcessRunning(const wchar_t* exeName);
void shellOpen(const std::wstring& target);
fs::path exeDir();
std::vector<unsigned char> resourceBytes(const wchar_t* name);  // RCDATA compiled into the exe (src/DSM.rc)
fs::path realCase(const fs::path& p);  // the path as stored on disk (Steam keeps it lowercase)

std::string readTextFile(const fs::path& p);
bool writeTextFile(const fs::path& p, const std::string& text);

std::string clockTime();              // "23:41:07"
std::string formatSize(uint64_t bytes); // "42.1 МБ"
