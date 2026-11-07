Birthday Adventure - source overview
====================================

HOW TO BUILD
1. Generate glad (OpenGL 3.3 Core, glad2) at https://gen.glad.sh and unzip it into
   third_party/glad  (needs third_party/glad/include/glad/gl.h and src/gl.c).
2. Put DejaVuSans.ttf, DejaVuSans-Bold.ttf, DejaVuSerif.ttf, DejaVuSerif-Bold.ttf and
   DejaVuSerif-Italic.ttf in assets/fonts/ (needed for text; the renderer also tries a
   few system fonts as a fallback).
3. cmake -S . -B build && cmake --build build
   (GLFW, GLM, FreeType, stb and miniaudio are downloaded automatically.)
4. Run build/birthday_adventure from the project folder. Edit birthday_config.json to
   personalise the names, messages and photos. Shaders in shaders/ are optional; an
   embedded copy is used when the files are missing. Audio files in assets/audio/
   (<name>.wav/.mp3/.flac) override the built-in generated sounds.

CONTROLS
WASD / arrows move, SHIFT runs, E / SPACE / ENTER interact, ESC pause, F1 warnings,
F2 screenshot, M mute.

FILES
Common.h/.cpp    math, RNG, logging, file/UTF-8 helpers, game-root path handling
Renderer.h/.cpp  OpenGL 3.3 batched 2D renderer, text, light map
Atlas.h/.cpp     procedural pixel-art sprite sheet
Audio.h/.cpp     procedural sound effects, music and ambience
Config.h/.cpp    birthday_config.json loader (built-in JSON reader) and save-game state
Entities.h/.cpp  player, companion, NPCs, chests, props, collectibles
World.h/.cpp     tile map, level layout, entity placement, drawing and lights
main.cpp         window, game loop, input, interaction/puzzle rules, UI, ending

GAME FLOW
Forest: find the Forest Key (search rustling bushes) -> forest gate.
Lake:   pull the lever to extend the bridge.
Garden: read the sign, touch the four glowing flowers in order -> Garden Key.
Castle: Garden Key opens the castle gate; the Golden Key is in the secret study
        (ask the ghost); it opens the throne room and the grand doors.
Final:  open every other chest, then the golden chest. Fireworks and the final message.

STATUS
All files compile together. The headers Audio.h, Entities.h, World.h, Config.h and the
files Config.cpp and main.cpp were written to match the existing .cpp files. They were
checked with a syntax/link test and a headless world-generation test, but the game has
not been run with a real window, GPU or audio device - expect to fix small things on
the first real build.
