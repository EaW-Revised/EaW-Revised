#pragma once
// The one platform call of the content digest: its worker thread runs below the game's own loading.
namespace eawr::mods {

// Lowers the calling thread's CPU (and on Windows background I/O and memory) priority. Best effort, never throws.
void lower_current_thread_priority();

} // namespace eawr::mods
