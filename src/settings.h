#pragma once

// Le um inteiro de "<nome do exe>.ini" (ao lado do exe; criado com os padroes se nao existir)
int SettingsGetInt(const wchar_t* section, const wchar_t* key, int defaultValue);
