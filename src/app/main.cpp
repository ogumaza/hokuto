// SPDX-License-Identifier: MIT

// The program's entry points. SDL3 runs the main loop and calls these functions from it. SDL3 keeps calling
// SDL_AppIterate while the user resizes the window. The window then redraws at each new size.

#define SDL_MAIN_USE_CALLBACKS 1

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <cstring>
#include <memory>
#include <string>

#include "app/app.h"

using hokuto::app::App;

SDL_AppResult SDL_AppInit(void** state, int argc, char* argv[])
{
    // `--preview` fills the zones with a made-up song. Another argument names a sound archive to open. SDL3 passes the
    // arguments in UTF-8 on every system.
    bool preview = false;
    std::string file;
    for (int i = 1; i < argc; i++)
    {
        if (std::strcmp(argv[i], "--preview") == 0)
        {
            preview = true;
        }
        else if (argv[i][0] != '-')
        {
            file = argv[i];
        }
    }

    std::unique_ptr<App> app = App::Create(preview, file);
    if (!app)
    {
        return SDL_APP_FAILURE;
    }

    *state = app.release();

    return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void* state, SDL_Event* event)
{
    return static_cast<App*>(state)->HandleEvent(*event);
}

SDL_AppResult SDL_AppIterate(void* state)
{
    return static_cast<App*>(state)->Iterate();
}

// SDL3 calls SDL_AppQuit after a failed SDL_AppInit too. `state` is then null.
void SDL_AppQuit(void* state, SDL_AppResult /*result*/)
{
    delete static_cast<App*>(state);
}
