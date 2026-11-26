#pragma once

#include <string>

namespace eawr::presentation::godot_backend {

// The viewer side of the telemetry feed `context` record (#8058, capture upgrade): the JSON object body, without braces, of
// what a reader needs to interpret a capture on another PC. Main thread only (it reads the display and renderer
// singletons). Aggregates only: the GPU model and driver version, the window and render settings, the detail choices and
// the thread counts. No host name, user name, path or process name.
//
//   gpu, gpu_vendor, gpu_type, gpu_api, gpu_driver, renderer, rd_driver    the graphics adapter as the engine reports it
//   res [w, h], window, vsync, fps_cap                                      window size and mode, vsync mode, engine FPS cap
//   render_thread, render_thread_setting                                    "separate" or "main" (the running mode) and the saved choice
//   video {...}                                                             the saved detail and render choices
//   threads {...}                                                           hardware threads, and per pool the saved choice,
//                                                                           the automatic count and whether it is Automatic
//
// The actual worker count of each running simulation is the `workers` field of its `source` record.
[[nodiscard]] std::string perf_context_fields();

} // namespace eawr::presentation::godot_backend
