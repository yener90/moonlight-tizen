#include "moonlight_wasm.hpp"

#include <cstdarg>
#include <cstring>
#include <string>

#include <emscripten/threading.h>

void MoonlightInstance::ClStageStarting(int stage) {
  PostToJs(std::string("ProgressMsg: Starting ") +
           std::string(LiGetStageName(stage)) + std::string("..."));
}

void MoonlightInstance::ClStageFailed(int stage, long errorCode) {
  PostToJs(std::string("DialogMsg: ") + std::string(LiGetStageName(stage)) +
           std::string(" failed (error ") + std::to_string(errorCode) +
           std::string(")"));
}

void MoonlightInstance::ClConnectionStarted(void) {
  emscripten_sync_run_in_main_runtime_thread(EM_FUNC_SIG_V,
                                             onConnectionStarted);
}

void MoonlightInstance::ClConnectionTerminated(long errorCode) {
  // Teardown the connection
  LiStopConnection();

  emscripten_sync_run_in_main_runtime_thread(EM_FUNC_SIG_VI,
                                             onConnectionStopped, errorCode);
}

void MoonlightInstance::ClDisplayMessage(const char* message) {
  PostToJs(std::string("DialogMsg: ") + std::string(message));
}

void MoonlightInstance::ClDisplayTransientMessage(const char* message) {
  PostToJs(std::string("TransientMsg: ") + std::string(message));
}

void onConnectionStarted() { g_Instance->OnConnectionStarted(0); }

void onConnectionStopped(long errorCode) {
  g_Instance->OnConnectionStopped(errorCode);
}

void MoonlightInstance::ClLogMessage(const char* format, ...) {
  va_list va;
  va_start(va, format);
  char buf[1000];
  vsnprintf(buf, sizeof(buf), format, va);
  va_end(va);

  // fprintf(stderr, ...) processes message in parts, so logs from different
  // threads may interleave. Send whole message at once to minimize this.
  fputs(buf, stderr);
}

CONNECTION_LISTENER_CALLBACKS MoonlightInstance::s_ClCallbacks = {
    .stageStarting = MoonlightInstance::ClStageStarting,
    .stageFailed = MoonlightInstance::ClStageFailed,
    .connectionStarted = MoonlightInstance::ClConnectionStarted,
    .connectionTerminated = MoonlightInstance::ClConnectionTerminated,
    .logMessage = MoonlightInstance::ClLogMessage,
};
