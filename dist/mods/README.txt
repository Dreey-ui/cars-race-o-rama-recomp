MODS FOLDER - Cars Race-O-Rama (recomp)

Anything placed here replaces the file with the same path in the game folder (the game files are never changed).
To undo a mod, delete its file from here.

Examples (path relative to the game folder):
  mods\C\Act\CS\CS_01\CS_01.as           -> cutscene 1 (INI text: camera, animations, timings)
  mods\C\Audio\Engine\BubEngineAudio.ini -> engine sound of a car
  mods\config.ini                        -> boot configuration (video mode, languages)
  mods\C\...\anything.dds / .xma / .wav  -> texture, music, sound
  mods\C\...\X.hidden                    -> hides the game file X (the game sees it as missing)

LUA SCRIPTS
  All 324 game scripts are already decompiled to text in the lua_src folder (next to the exe).
  To change a script: copy the .lua from there to here at the same path and edit it. Example:
    copy  lua_src\C\Script\defs.lua   to   mods\C\Script\defs.lua
  The game uses your .lua instead of the original .lua.bin (the game's own Lua compiles the text).
  The game's Lua is 5.1 (with float numbers, and it accepts "!=" as well as "~="). Do not use goto (that is Lua 5.2).
  Syntax errors show up in the log (logs\<date>\game.log, next to the exe) as:  [lua] syntax error in ..., line N: ...
  Each script loaded from here shows up in the log as:  [mods] ...

SCENES AS LOOSE FILES (mods\C\Scenes)
  The game does not use the scene packages (C\Scenes\**\*.x360.res and *.x360.str): it reads the loose files of each
  scene, in the same layout as the development scenes:
    <Scene>\geo\*.xng    models            <Scene>\tex\*.dds  textures (PC DDS)       <Scene>\tex\*.xnm  materials (text)
    <Scene>\col\*.gol    collisions        Eco, Anim, Skies, MiniMap, ShadowMap, .scn, .toy, .aud ...
    <Scene>\_package\    package skeleton (.soi/.toc: zones, object placement) - do NOT delete or edit
  When the scene opens, the runtime builds the package from these files (models, textures and collisions are turned
  back into the Xbox 360 format on the fly). Restart the game to see a change.
  EDITING: replace the .xng / .dds / .gol / .xnm keeping the name. You can change the vertex count of a model, the size
  and mipmaps of a texture (DXT1/DXT3/DXT5 or ARGB 8888) and the collision. NEW objects are not supported yet (the scene
  only has the skeleton's objects). The session log (logs\<date>\game.log) lists each modified file:
  "[mods] <package>: <file> (modified)".
  If a file is missing or invalid: "[mods] ERROR loose scene ...: <reason>" and the game uses the original package.
  To go back to the original packages: delete the mods\C\Scenes folder. Copy of the originals: _backup_original\C\Scenes.
  Clean copy of the extracted scenes: extracted_scenes (next to the exe). Extract again: python tools\scenes_decompile.py.

SAVES AND PROFILE
  The game starts signed in to a local profile (the name is your Windows user name; to change it, edit
  savegame\profile.ini, line Name=). Progress is kept in the savegame folder, inside the game folder:
    PSLOT0.dat\ ... (one per story mode save)   GlobalData\ (Hall of Fame)
  To back up, copy the savegame folder. To start over, delete it (with the game closed).

TEXTS WITHOUT "XBOX 360"
  mods\E\Loc (and F, G, I, S for the other languages): the messages that mentioned the Xbox 360 console say "computer"
  ("Please don't turn off your computer") and the controller ones just say "controller". To get the original texts
  back, delete those files (or: python tools\pc_texts.py --undo, in the runtime project).

DIAGNOSTICS
  F12 (with the game window focused): marks "something odd" in the session log (logs folder next to the exe):
  screenshots of the last 5 s, the audio of the last 10 s and the complete next frame in logs\<date>\mark_NN.
