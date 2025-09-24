#ifndef FILESYSTEM_H
#define FILESYSTEM_H

#include "SDL3/SDL.h"

void SDL_InitFS(void);

void listFiles(const char *dirname);

#endif // FILESYSTEM_H
