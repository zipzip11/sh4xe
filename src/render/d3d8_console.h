#pragma once

namespace sh4xe::render::d3d8_console
{

void AddLog(const char* fmt, ...);
void Render(void* device);

// True while the overlay console is open. Used by the input hook to swallow the
// game's DirectInput so keystrokes typed into the console do not also drive the
// game.
bool IsOpen();

} // namespace sh4xe::render::d3d8_console
