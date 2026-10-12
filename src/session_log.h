#pragma once
// Session log: every time the game starts it creates <exe folder>\logs\YYYY-MM-DD_HH-MM-SS\ with game.log (everything the
// runtime writes to stdout/stderr, with the time on each line). F12 creates mark_NN\ inside it (screenshots, audio, frame trace).
#include <cstdint>
#include <filesystem>

void SessionLogStart();                                   // first thing in main
const std::filesystem::path& SessionDir();                // empty if the folder could not be created
void SessionLogFlush();                                   // waits until everything reaches the file (crash, exit)
void SessionLogEnd(const char* reason);                   // final "[session] end" line + flush
std::filesystem::path SessionNewMark(int& index);         // creates mark_NN_HH-MM-SS and returns its path
// 24-bit BGR image (rows top to bottom) as PNG or JPEG (WIC)
bool WriteImageFile(const std::filesystem::path& path, const uint8_t* bgr, uint32_t w, uint32_t h, uint32_t stride, bool jpeg);
