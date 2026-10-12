# Reescreve os 4 trechos em que o unluac gerou goto (Lua 5.2), que o Lua 5.1 do jogo nao aceita.
# Todos sao "if a ~= X and b ~= Y then pula" (um "or" que o descompilador nao reconstruiu).
# Uso: python -I tools/fix_lua_goto.py lua_src
import os
import sys

root = sys.argv[1]

PLACED_OLD = """  if L1_2 ~= L2_2 then
    L1_2 = StoryEvent_GetStatus
    L2_2 = A0_2
    L1_2 = L1_2(L2_2)
    L2_2 = StoryEventStatus
    L3_2 = "Placed"
    L2_2 = L2_2(L3_2)
    if L1_2 ~= L2_2 then
      goto lbl_20
    end
  end
  L1_2 = true
  do return L1_2 end
  goto lbl_22
  ::lbl_20::
  L1_2 = false
  do return L1_2 end
  ::lbl_22::
end"""
PLACED_NEW = """  if L1_2 == L2_2 then
    return true
  end
  L1_2 = StoryEvent_GetStatus
  L2_2 = A0_2
  L1_2 = L1_2(L2_2)
  L2_2 = StoryEventStatus
  L3_2 = "Placed"
  L2_2 = L2_2(L3_2)
  if L1_2 == L2_2 then
    return true
  end
  return false
end"""

INTRO_OLD = """    L2_2 = GetPlatform
    L2_2 = L2_2()
    if L2_2 ~= "x360" then
      L2_2 = GetPlatform
      L2_2 = L2_2()
      if L2_2 ~= "ps3" then
        goto lbl_17
      end
    end
    A0_2.srcMovieDir = "C\\\\Movies\\\\720\\\\"
    goto lbl_24
    ::lbl_17::
    L2_2 = IsDisplayModePAL
    L2_2 = L2_2()
    if L2_2 then
      A0_2.srcMovieDir = "C\\\\Movies\\\\2500\\\\"
    else
      A0_2.srcMovieDir = "C\\\\Movies\\\\2997\\\\"
    end
    ::lbl_24::"""
INTRO_NEW = """    L2_2 = GetPlatform
    L2_2 = L2_2()
    if L2_2 == "x360" or GetPlatform() == "ps3" then
      A0_2.srcMovieDir = "C\\\\Movies\\\\720\\\\"
    else
      L2_2 = IsDisplayModePAL
      L2_2 = L2_2()
      if L2_2 then
        A0_2.srcMovieDir = "C\\\\Movies\\\\2500\\\\"
      else
        A0_2.srcMovieDir = "C\\\\Movies\\\\2997\\\\"
      end
    end"""

CTMD_OLD = """  L1_2 = A0_2.GestureSet
  if L1_2 ~= "x360" then
    L1_2 = A0_2.GestureSet
    if L1_2 ~= "x360_wheel" then
      goto lbl_46
    end
  end
  L1_2 = GetControllerConfig
  L2_2 = 1
  L1_2 = L1_2(L2_2)
  if L1_2 == 2 then
    A0_2.GestureSet = "x360_wheel"
  end
  ::lbl_46::"""
CTMD_NEW = """  L1_2 = A0_2.GestureSet
  if L1_2 == "x360" or A0_2.GestureSet == "x360_wheel" then
    L1_2 = GetControllerConfig
    L2_2 = 1
    L1_2 = L1_2(L2_2)
    if L1_2 == 2 then
      A0_2.GestureSet = "x360_wheel"
    end
  end"""

fixes = {
    os.path.join("C", "Script", "defs.lua"): [(PLACED_OLD, PLACED_NEW)],
    os.path.join("C", "Story", "defs.lua"): [(PLACED_OLD, PLACED_NEW)],
    os.path.join("C", "Act", "FE", "Intro", "FE_Intro.lua"): [(INTRO_OLD, INTRO_NEW)],
    os.path.join("C", "Act", "MG_CT_MD", "MG_CT_MD.lua"): [(CTMD_OLD, CTMD_NEW)],
}
for rel, reps in fixes.items():
    path = os.path.join(root, rel)
    text = open(path, encoding="latin-1").read().replace("\r\n", "\n")
    for old, new in reps:
        if old not in text:
            print("NAO ACHEI o trecho em", rel)
            continue
        text = text.replace(old, new)
    open(path, "w", encoding="latin-1", newline="\n").write(text)
    left = text.count("goto ")
    print("%s: %s" % (rel, "ok" if left == 0 else "%d goto restantes" % left))
