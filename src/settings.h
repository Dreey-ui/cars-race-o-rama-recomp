#pragma once

// Reads an integer from "<exe name>.ini" (next to the exe; created with the defaults if missing)
int SettingsGetInt(const wchar_t* section, const wchar_t* key, int defaultValue);
