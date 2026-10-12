// Configuracoes do usuario: "<nome do exe>.ini" ao lado do executavel (criado com os valores padrao na primeira vez)
#include "settings.h"

#include <windows.h>

#include <filesystem>
#include <string>

static std::wstring IniPath()
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return std::filesystem::path(exe).replace_extension(L".ini").wstring();
}

static const char kDefaultIni[] =
    "; Configuracoes do Cars Race-O-Rama (recomp)\r\n"
    "[Video]\r\n"
    "; FPS maximo: 60 (destravado) ou 30 (original do Xbox 360)\r\n"
    "FPS=60\r\n"
    "; VSync: 1 = ligado (sem tearing), 0 = desligado\r\n"
    "VSync=1\r\n";

int SettingsGetInt(const wchar_t* section, const wchar_t* key, int defaultValue)
{
    static const std::wstring path = [] {
        const std::wstring p = IniPath();
        if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            if (FILE* f = _wfopen(p.c_str(), L"wb"))
            {
                fwrite(kDefaultIni, 1, sizeof(kDefaultIni) - 1, f);
                fclose(f);
            }
        }
        return p;
    }();
    return int(GetPrivateProfileIntW(section, key, defaultValue, path.c_str()));
}
