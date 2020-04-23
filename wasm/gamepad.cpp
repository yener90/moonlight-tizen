#include "moonlight_wasm.hpp"

#include <iostream>
#include <array>
#include <utility>

#include <Limelight.h>
#include <emscripten/emscripten.h>

// For explanation on ordering, see: https://www.w3.org/TR/gamepad/#remapping
enum GamepadAxis {
  LeftX = 0,
  LeftY = 1,
  RightX = 2,
  RightY = 3,
};

enum GamepadButton {
  A, B, X, Y,
  LeftShoulder, RightShoulder,
  LeftTrigger, RightTrigger,
  Back, Play,
  LeftStick, RightStick,
  Up, Down, Left, Right,
  Special,
  Count,
};

static short GetActiveGamepadMask(int numGamepads) {
  short result = 0;
  for (int i = 0; i < numGamepads; ++i) {
    result |= (1 << i);
  }
  return result;
}

static short GetButtonFlags(const EmscriptenGamepadEvent& gamepad) {
  // Triggers are considered analog buttons in Emscripten API, however they
  // need to be passed in separate arguments for Limelight (it even lacks flags
  // for them).
  static const int buttonMasks[] {
      A_FLAG, B_FLAG, X_FLAG, Y_FLAG,
      LB_FLAG, RB_FLAG,
      0 /* LTRIGGER */, 0 /* RTRIGGER */,
      BACK_FLAG, PLAY_FLAG,
      LS_CLK_FLAG, RS_CLK_FLAG,
      UP_FLAG, DOWN_FLAG, LEFT_FLAG, RIGHT_FLAG,
      SPECIAL_FLAG,
  };
  static const int buttonMasksSize =
    static_cast<int>(sizeof(buttonMasks) / sizeof(buttonMasks[0]));

  short result = 0;
  for (int i = 0; i < gamepad.numButtons && i < buttonMasksSize; ++i) {
    if (gamepad.digitalButton[i] == EM_TRUE) {
      result |= buttonMasks[i];
    }
  }
  return result;
}

void MoonlightInstance::PollGamepads() {
  if (emscripten_sample_gamepad_data() != EMSCRIPTEN_RESULT_SUCCESS) {
    std::cerr << "Sample gamepad data failed!\n";
    return;
  }
  const auto numGamepads = emscripten_get_num_gamepads();
  if (numGamepads == EMSCRIPTEN_RESULT_NOT_SUPPORTED) {
    std::cerr << "Get num gamepads failed!\n";
    return;
  }
  const auto activeGamepadMask = GetActiveGamepadMask(numGamepads);
  for (int gamepadID = 0; gamepadID < numGamepads; ++gamepadID) {
    emscripten_sample_gamepad_data();
    EmscriptenGamepadEvent gamepad;
    const auto result = emscripten_get_gamepad_status(gamepadID,
                                                      &gamepad);
    if (result != EMSCRIPTEN_RESULT_SUCCESS || !gamepad.connected) {
      continue;
    }

    const auto buttonFlags = GetButtonFlags(gamepad);
    const auto leftTrigger = gamepad.analogButton[GamepadButton::LeftTrigger]
      * std::numeric_limits<unsigned char>::max();
    const auto rightTrigger = gamepad.analogButton[GamepadButton::RightTrigger]
      * std::numeric_limits<unsigned char>::max();
    const auto leftStickX = gamepad.axis[GamepadAxis::LeftX]
      * std::numeric_limits<short>::max();
    const auto leftStickY = -gamepad.axis[GamepadAxis::LeftY]
      * std::numeric_limits<short>::max();
    const auto rightStickX = gamepad.axis[GamepadAxis::RightX]
      * std::numeric_limits<short>::max();
    const auto rightStickY = -gamepad.axis[GamepadAxis::RightY]
      * std::numeric_limits<short>::max();

    LiSendMultiControllerEvent(
        gamepadID, activeGamepadMask, buttonFlags, leftTrigger,
        rightTrigger, leftStickX, leftStickY, rightStickX, rightStickY);
  }
}
