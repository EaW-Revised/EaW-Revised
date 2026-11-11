#pragma once
// The one platform call of the content digest: its worker thread runs below the game's own loading.
namespace eawr::mods {

// Lowers the calling thread's CPU (and on Windows background I/O and memory) priority. Best effort, never throws.
void lower_current_thread_priority();
// Reversibly switch the calling hash worker between background and normal priority.
// background_io false keeps the I/O and memory priority normal and lowers only the CPU priority (below normal):
// Windows background mode makes cold reads about six times slower and spends about four times the CPU in the kernel.
void set_hash_thread_background(bool background, bool background_io = true);
unsigned long current_process_id();

} // namespace eawr::mods
