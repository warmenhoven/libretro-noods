#include <cstdarg>
#include <algorithm>
#include <cstring>
#include <regex>
#include <chrono>

#include <fcntl.h>
#include <fstream>
#include <sstream>

#include "libretro.h"
#include "savestate.h"

#include "../common/screen_layout.h"
#include "../settings.h"
#include "../core.h"
#include "../defines.h"

#ifndef VERSION
#define VERSION "0.1"
#endif

static retro_environment_t envCallback;
static retro_video_refresh_t videoCallback;
static retro_audio_sample_batch_t audioBatchCallback;
static retro_input_poll_t inputPollCallback;
static retro_input_state_t inputStateCallback;

static struct retro_log_callback logging;
static retro_log_printf_t logCallback;

static retro_microphone_t* microphone;
static retro_microphone_interface micInterface;
static bool micAvailable;

static std::string systemPath;
static std::string savesPath;

static Core *core;
static ScreenLayout layout;
static ScreenLayout touch;

static std::string ndsPath;
static std::string gbaPath;

static int ndsSaveFd = -1;
static int gbaSaveFd = -1;

static std::vector<uint32_t> videoBuffer;
static uint32_t videoBufferSize;

static std::string micInputMode;
static std::string micButtonMode;

static std::string touchMode;
static std::string screenSwapMode;

static int screenArrangement;
static int screenRotation;
static int screenPosition;

static bool gbaModeEnabled;
static bool renderGbaScreen;
static bool renderTopScreen;
static bool renderBotScreen;

static bool micToggled;
static bool micActive;

static bool showTouchCursor;
static bool screenSwapped;
static bool swapScreens;
static bool screenTouched;

static auto cursorTimeout = 0;
static auto cursorMovedAt = std::chrono::steady_clock::now();
static bool cursorVisible = false;

static int lastMouseX = 0;
static int lastMouseY = 0;

static int touchX = 0;
static int touchY = 0;

static int keymap[] = {
  RETRO_DEVICE_ID_JOYPAD_A,
  RETRO_DEVICE_ID_JOYPAD_B,
  RETRO_DEVICE_ID_JOYPAD_SELECT,
  RETRO_DEVICE_ID_JOYPAD_START,
  RETRO_DEVICE_ID_JOYPAD_RIGHT,
  RETRO_DEVICE_ID_JOYPAD_LEFT,
  RETRO_DEVICE_ID_JOYPAD_UP,
  RETRO_DEVICE_ID_JOYPAD_DOWN,
  RETRO_DEVICE_ID_JOYPAD_R,
  RETRO_DEVICE_ID_JOYPAD_L,
  RETRO_DEVICE_ID_JOYPAD_X,
  RETRO_DEVICE_ID_JOYPAD_Y
};

static int arrangeMap [] = {
  0, // 0 - Automatic      0 - Automatic
  2, // 1 - Vertical       2 - Horizontal
  1, // 2 - Horizontal     1 - Vertical
  3, // 3 - Single Screen  3 - Single Screen
};

static int rotationMap [] = {
  0, // 0 - Normal        0 - Normal
  2, // 1 - RotatedLeft   2 - Counter-clockwise
  0, // 2 - UpsideDown    0 - Normal
  1, // 3 - RotatedRight  1 - Clockwise
};

static int positionMap [4][3] = {
  { 0, 3, 4 }, // Automatic     | Center, Left, Right
  { 0, 3, 4 }, // Vertical      | Center, Left, Right
  { 0, 1, 2 }, // Horizontal    | Center, Top, Bottom
  { 0, 0, 0 }, // Single Screen | Center, Center, Center
};

static int viewPositionMap [4][5] = {
  { 0, 1, 2, 3, 4 }, // 0 - Normal       | No change
  { 0, 1, 2, 4, 3 }, // 1 - RotatedLeft  | Center, Top, Bottom, Right, Left
  { 0, 1, 2, 3, 4 }, // 2 - UpsideDown   | No change
  { 0, 2, 1, 3, 4 }, // 3 - RotatedRight | Center, Bottom, Top, Left, Right
};

static int touchPositionMap [4][5] = {
  { 0, 1, 2, 3, 4 }, // 0 - Normal       | No change
  { 0, 3, 4, 2, 1 }, // 1 - RotatedLeft  | Center, Left, Right, Bottom, Top
  { 0, 1, 2, 3, 4 }, // 2 - UpsideDown   | No change
  { 0, 4, 3, 1, 2 }, // 3 - RotatedRight | Center, Right, Left, Top, Bottom
};

static int32_t clampValue(int32_t value, int32_t min, int32_t max)
{
  return std::max(min, std::min(max, value));
}

static bool endsWith(std::string str, std::string end)
{
  return str.find(end, str.length() - end.length()) != std::string::npos;
}

static std::string normalizePath(std::string path, bool addSlash = false)
{
  std::string newPath = path;
  if (addSlash && newPath.back() != '/') newPath += '/';
#ifdef WINDOWS
  std::replace(newPath.begin(), newPath.end(), '\\', '/');
#endif
  return newPath;
}

static std::string getNameFromPath(std::string path)
{
  std::string base = path.substr(path.find_last_of("/\\") + 1);
  for (const auto& delim : {".zip#", ".7z#", ".apk#"})
  {
    size_t delimPos = base.find(delim);
    if (delimPos != std::string::npos) base = base.substr(0, delimPos);
  }
  return base.substr(0, base.rfind("."));
}

static void swapScreenPositions(ScreenLayout& sl)
{
  std::swap(sl.topWidth, sl.botWidth);
  std::swap(sl.topHeight, sl.botHeight);
  std::swap(sl.topX, sl.botX);
  std::swap(sl.topY, sl.botY);
}

static void logFallback(enum retro_log_level level, const char *fmt, ...)
{
  (void)level;
  va_list va;
  va_start(va, fmt);
  vfprintf(stderr, fmt, va);
  va_end(va);
}

static std::string fetchVariable(std::string key, std::string def)
{
  struct retro_variable var = { nullptr };
  var.key = key.c_str();

  if (!envCallback(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value == nullptr)
  {
    logCallback(RETRO_LOG_WARN, "Fetching variable %s failed.", var.key);
    return def;
  }

  return std::string(var.value);
}

static bool fetchVariableBool(std::string key, bool def)
{
  return fetchVariable(key, def ? "enabled" : "disabled") == "enabled";
}

static int fetchVariableInt(std::string key, int def)
{
  std::string value = fetchVariable(key, std::to_string(def));

  if (!value.empty() && std::isdigit(value[0]))
    return std::stoi(value);

  return 0;
}

static int fetchVariableEnum(std::string key, std::vector<std::string> list, int def = 0)
{
  auto val = fetchVariable(key, list[def]);
  auto itr = std::find(list.begin(), list.end(), val);

  return std::distance(list.begin(), itr);
}

static std::string getSaveDir()
{
  char* dir = nullptr;
  if (!envCallback(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir) || dir == nullptr)
  {
    logCallback(RETRO_LOG_INFO, "No save directory provided by LibRetro.");
    return std::string("NooDS");
  }
  return std::string(dir);
}

static std::string getSystemDir()
{
  char* dir = nullptr;
  if (!envCallback(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) || dir == nullptr)
  {
    logCallback(RETRO_LOG_INFO, "No system directory provided by LibRetro.");
    return std::string("NooDS");
  }
  return std::string(dir);
}

static bool getButtonState(unsigned id)
{
  return inputStateCallback(0, RETRO_DEVICE_JOYPAD, 0, id);
}

static float getAxisState(unsigned index, unsigned id)
{
  return inputStateCallback(0, RETRO_DEVICE_ANALOG, index, id);
}

static void initInput(void)
{
  static const struct retro_controller_description controllers[] = {
    { "Nintendo DS", RETRO_DEVICE_JOYPAD },
    { NULL, 0 },
  };

  static const struct retro_controller_info ports[] = {
    { controllers, 1 },
    { NULL, 0 },
  };

  envCallback(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*)ports);

  struct retro_input_descriptor desc[] = {
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "Left" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "Up" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "Down" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "A" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "B" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "R" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "L" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "X" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "Y" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2, "Microphone" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2, "Swap screens" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3, "Touch joystick" },
    { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Touch joystick X" },
    { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Touch joystick Y" },
    { 0 },
  };

  envCallback(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, &desc);
}

static void initConfig()
{
  static const retro_variable values[] = {
    { "noods_directBoot", "Direct Boot; enabled|disabled" },
    { "noods_fpsLimiter", "FPS Limiter; disabled|enabled" },
    { "noods_romInRam", "Keep ROM in RAM; disabled|enabled" },
    { "noods_dsiMode", "DSi Homebrew Mode; disabled|enabled" },
    { "noods_threaded2D", "Threaded 2D; enabled|disabled" },
    { "noods_threaded3D", "Threaded 3D; 1 Thread|2 Threads|3 Threads|4 Threads|Disabled" },
    { "noods_highRes3D", "High Resolution 3D; disabled|enabled" },
    { "noods_screenArrangement", "Screen Arrangement; Automatic|Vertical|Horizontal|Single Screen" },
    { "noods_screenRotation", "Screen Rotation; Normal|Rotated Left|Rotated Right" },
    { "noods_screenSizing", "Screen Sizing; Even|Enlarge Top|Enlarge Bottom" },
    { "noods_screenPosition", "Screen Position; Center|Start|End" },
    { "noods_screenGap", "Screen Gap; None|Quarter|Half|Full" },
    { "noods_gbaCrop", "Crop GBA Screen; enabled|disabled" },
    { "noods_screenFilter", "Screen Filter; Nearest|Upscaled|Linear" },
    { "noods_screenGhost", "Simulate Ghosting; disabled|enabled" },
    { "noods_swapScreenMode", "Swap Screen Mode; Toggle|Hold" },
    { "noods_touchMode", "Touch Mode; Auto|Pointer|Joystick|None" },
    { "noods_touchCursor", "Show Touch Cursor; enabled|disabled" },
    { "noods_cursorTimeout", "Hide Cursor Timeout; 3 Seconds|5 Seconds|10 Seconds|15 Seconds|20 Seconds|Never Hide" },
    { "noods_micInputMode", "Microphone Input Mode; Silence|Noise|Microphone" },
    { "noods_micButtonMode", "Microphone Button Mode; Toggle|Hold|Always" },
    { nullptr, nullptr }
  };

  envCallback(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)values);
}

static void updateConfig()
{
  Settings::basePath = savesPath + "noods";
  Settings::bios9Path = systemPath + "bios9.bin";
  Settings::bios7Path = systemPath + "bios7.bin";
  Settings::firmwarePath = systemPath + "firmware.bin";
  Settings::gbaBiosPath = systemPath + "gba_bios.bin";
  Settings::sdImagePath = systemPath + "nds_sd_card.bin";

  Settings::directBoot = fetchVariableBool("noods_directBoot", true);
  Settings::fpsLimiter = fetchVariableBool("noods_fpsLimiter", false);
  Settings::romInRam = fetchVariableBool("noods_romInRam", false);
  Settings::dsiMode = fetchVariableBool("noods_dsiMode", false);
  Settings::threaded2D = fetchVariableBool("noods_threaded2D", true);
  Settings::threaded3D = fetchVariableEnum("noods_threaded3D", {"Disabled", "1 Thread", "2 Threads", "3 Threads", "4 Threads"}, 1);
  Settings::highRes3D = fetchVariableBool("noods_highRes3D", false);
  Settings::screenFilter = fetchVariableEnum("noods_screenFilter", {"Nearest", "Upscaled", "Linear"});
  Settings::screenGhost = fetchVariableBool("noods_screenGhost", false);

  micInputMode = fetchVariable("noods_micInputMode", "Silence");
  micButtonMode = fetchVariable("noods_micButtonMode", "Toggle");

  screenArrangement = fetchVariableEnum("noods_screenArrangement", {"Automatic", "Vertical", "Horizontal", "Single Screen"});
  screenRotation = fetchVariableEnum("noods_screenRotation", {"Normal", "Rotated Left", "Upside Down", "Rotated Right"});
  screenPosition = fetchVariableEnum("noods_screenPosition", {"Center", "Start", "End"});

  screenSwapMode = fetchVariable("noods_swapScreenMode", "Toggle");
  touchMode = fetchVariable("noods_touchMode", "Touch");
  showTouchCursor = fetchVariableBool("noods_touchCursor", true);
  cursorTimeout = fetchVariableInt("noods_cursorTimeout", 3);

  ScreenLayout::gbaCrop = fetchVariableBool("noods_gbaCrop", true);
  ScreenLayout::screenSizing = fetchVariableEnum("noods_screenSizing", {"Even", "Enlarge Top", "Enlarge Bottom"});
  ScreenLayout::screenGap = fetchVariableEnum("noods_screenGap", {"None", "Quarter", "Half", "Full"});

  envCallback(RETRO_ENVIRONMENT_SET_ROTATION, &screenRotation);
}

static void updateScreenLayout()
{
  int screenSizing = 0; int screenWidth = 0; int screenHeight = 0;

  if (screenArrangement == 1 && screenRotation && ScreenLayout::screenSizing)
  {
    screenSizing = ScreenLayout::screenSizing;
    ScreenLayout::screenSizing = screenSizing == 2 ? 1 : 2;
  }

  ScreenLayout::screenArrangement = screenRotation ? arrangeMap[screenArrangement] : screenArrangement;
  ScreenLayout::screenRotation = rotationMap[0];
  ScreenLayout::screenPosition = positionMap[ScreenLayout::screenArrangement][screenPosition];
  ScreenLayout::screenPosition = viewPositionMap[screenRotation][ScreenLayout::screenPosition];

  layout.update(0, 0, gbaModeEnabled, false);

  if (ScreenLayout::screenSizing && ScreenLayout::screenArrangement == 2)
  {
    screenWidth = layout.minWidth / 2 * 3;
    screenHeight = layout.minHeight * 2;
  }

  if (ScreenLayout::screenSizing && ScreenLayout::screenArrangement < 2)
  {
    screenWidth = layout.minWidth * 2;
    screenHeight = layout.minHeight / 2 * 3;
  }

  if (screenWidth && screenHeight)
  {
    ScreenLayout::integerScale = true;
    layout.update(screenWidth, screenHeight, gbaModeEnabled, false);

    layout.minWidth = layout.winWidth;
    layout.minHeight = layout.winHeight;
  }

  if (screenArrangement == 1 && screenRotation)
  {
    ScreenLayout::screenSizing = screenSizing;
    swapScreenPositions(layout);
  }

  ScreenLayout::screenArrangement = screenArrangement;
  ScreenLayout::screenRotation = rotationMap[screenRotation];
  ScreenLayout::screenPosition = touchPositionMap[screenRotation][ScreenLayout::screenPosition];

  touch.update(0, 0, gbaModeEnabled, false);

  if (screenWidth && screenHeight)
  {
    if (ScreenLayout::screenRotation) std::swap(screenWidth, screenHeight);
    touch.update(screenWidth, screenHeight, gbaModeEnabled, false);

    touch.minWidth = touch.winWidth;
    touch.minHeight = touch.winHeight;
  }

  bool shift = Settings::highRes3D || Settings::screenFilter == 1;
  auto bsize = (layout.minWidth << shift) * (layout.minHeight << shift);

  if (videoBufferSize != bsize)
  {
    videoBuffer.resize(bsize);
    videoBufferSize = bsize;
  }

  memset(videoBuffer.data(), 0, videoBuffer.size() * sizeof(videoBuffer[0]));
}

static void checkConfigVariables()
{
  bool updated = false;
  envCallback(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated);

  if (core && gbaModeEnabled != core->gbaMode)
  {
    gbaModeEnabled = core->gbaMode;
    updated = true;
  }

  if (updated)
  {
    updateConfig();
    updateScreenLayout();

    retro_system_av_info info;
    retro_get_system_av_info(&info);
    envCallback(RETRO_ENVIRONMENT_SET_GEOMETRY, &info);
  }
}

static void updateScreenState()
{
  bool singleScreen = ScreenLayout::screenArrangement == 3;
  auto screenSizing = ScreenLayout::screenSizing;

  if (screenSizing <= 1 && swapScreens)
    screenSizing = 2;
  else if (screenSizing == 2 && swapScreens)
    screenSizing = 1;

  renderGbaScreen = gbaModeEnabled && ScreenLayout::gbaCrop;
  renderTopScreen = !renderGbaScreen && (!singleScreen || screenSizing <= 1);
  renderBotScreen = !renderGbaScreen && (!singleScreen || screenSizing == 2);
}

static void drawCursor(uint32_t *data, int32_t pointX, int32_t pointY, int32_t size = 2)
{
  bool shift = Settings::highRes3D || Settings::screenFilter == 1;
  auto scale = layout.botWidth / 256;

  uint32_t posX = clampValue(pointX, size, (layout.botWidth / scale) - size);
  uint32_t posY = clampValue(pointY, size, (layout.botHeight / scale) - size);

  uint32_t minX = layout.botX << shift;
  uint32_t maxX = layout.minWidth << shift;

  uint32_t minY = layout.botY << shift;
  uint32_t maxY = layout.minHeight << shift;

  uint32_t curX = (layout.botX + (posX * scale)) << shift;
  uint32_t curY = (layout.botY + (posY * scale)) << shift;

  uint32_t cursorSize = (size * scale) << shift;

  uint32_t startY = clampValue(curY - cursorSize, minY, maxY);
  uint32_t endY = clampValue(curY + cursorSize, minY, maxY);

  uint32_t startX = clampValue(curX - cursorSize, minX, maxX);
  uint32_t endX = clampValue(curX + cursorSize, minX, maxX);

  for (uint32_t y = startY; y < endY; y++)
  {
    for (uint32_t x = startX; x < endX; x++)
    {
      uint32_t& pixel = data[(y * maxX) + x];
      pixel = (0xFFFFFF - pixel) | 0xFF000000;
    }
  }
}

static void copyScreen(uint32_t *src, uint32_t *dst, int sw, int sh, int dx, int dy, int dw, int dh, int stride)
{
  int scaleX = dw / sw;
  int scaleY = dh / sh;

  if ((scaleX >= 1 && scaleY >= 1) && (scaleX > 1 || scaleY > 1))
  {
    for (int y = 0; y < dh; ++y)
    {
      int srcY = (y / scaleY) * sw;
      int dstY = (dy + y) * stride + dx;

      for (int x = 0; x < dw; ++x)
        dst[dstY + x] = src[srcY + (x / scaleX)];
    }
  }
  else if (dx == 0 && dw == stride)
  {
    int pixels = dw * dh * sizeof(uint32_t);
    int offset = dy * stride + dx;

    memcpy(dst + offset, src, pixels);
  }
  else
  {
    int rowSize = dw * sizeof(uint32_t);

    for (int y = 0; y < dh; ++y)
    {
      int srcY = y * sw;
      int dstY = (dy + y) * stride + dx;

      memcpy(dst + dstY, src + srcY, rowSize);
    }
  }
}

static void renderVideo()
{
  bool shift = Settings::highRes3D || Settings::screenFilter == 1;
  auto width = layout.minWidth << shift;
  auto height = layout.minHeight << shift;

  static uint32_t buffer[256 * 192 * 8];
  core->gpu.getFrame(buffer, renderGbaScreen);

  if (renderGbaScreen)
  {
    copyScreen(
      &buffer[0], videoBuffer.data(),
      240 << shift, 160 << shift,
      layout.topX << shift, layout.topY << shift,
      layout.topWidth << shift, layout.topHeight << shift,
      width
    );
  }

  if (renderTopScreen)
  {
    copyScreen(
      &buffer[0], videoBuffer.data(),
      256 << shift, 192 << shift,
      layout.topX << shift, layout.topY << shift,
      layout.topWidth << shift, layout.topHeight << shift,
      width
    );
  }

  if (renderBotScreen)
  {
    copyScreen(
      &buffer[(256 * 192) << (shift * 2)], videoBuffer.data(),
      256 << shift, 192 << shift,
      layout.botX << shift, layout.botY << shift,
      layout.botWidth << shift, layout.botHeight << shift,
      width
    );

    if (showTouchCursor && cursorVisible)
      drawCursor(videoBuffer.data(), touchX, touchY);
  }

  videoCallback(videoBuffer.data(), width, height, width * 4);
}

static void renderAudio()
{
  static int16_t buffer[547 * 2];
  uint32_t *original = core->spu.getSamples(547);

  for (int i = 0; i < 547; i++)
  {
    buffer[i * 2 + 0] = original[i] >>  0;
    buffer[i * 2 + 1] = original[i] >> 16;
  }
  delete[] original;

  uint32_t size = sizeof(buffer) / (2 * sizeof(int16_t));
  audioBatchCallback(buffer, size);
}

static void openMicrophone()
{
  if (micAvailable && !microphone)
  {
    retro_microphone_params_t params = { 44100 };
    microphone = micInterface.open_mic(&params);

    micInterface.set_mic_state(microphone, false);
  }
}

static void closeMicrophone()
{
  if (micAvailable && microphone)
  {
    micInterface.close_mic(microphone);
    microphone = nullptr;
  }
}

static void setMicrophoneState(bool enabled)
{
  if (micInputMode == "Microphone" && micAvailable && microphone)
    micInterface.set_mic_state(microphone, enabled);
}

static void sendMicSamples()
{
  static const size_t maxSamples = 735;
  static int16_t buffer[maxSamples];

  size_t samplesRead = 0;

  if (micInputMode == "Microphone" && microphone && micInterface.get_mic_state(microphone))
  {
    samplesRead = micInterface.read_mic(microphone, buffer, maxSamples);
  }
  else if (micInputMode == "Noise")
  {
    samplesRead = maxSamples;
    for (int i = 0; i < maxSamples; i++) buffer[i] = rand() & 0xFFFF;
  }
  else
  {
    samplesRead = 0;
    memset(buffer, 0, sizeof(buffer));
  }

  if (samplesRead)
    core->spi.sendMicData(buffer, samplesRead, 44100);
}

static void updateCursorState()
{
  if (showTouchCursor && cursorTimeout)
  {
    if (cursorVisible)
    {
      auto current = std::chrono::steady_clock::now();
      auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(current - cursorMovedAt).count();

      if (elapsed >= cursorTimeout) cursorVisible = false;
    }
  }
  else
  {
    cursorVisible = true;
  }
}

static int getSaveFileDesc(std::string romPath)
{
  std::string path = savesPath + getNameFromPath(romPath) + ".sav";
  int fd = open(path.c_str(), O_RDWR);
  if (fd == -1)
  {
    std::ofstream file(path, std::ios::binary);
    if (file.is_open())
    {
      file.put(0xFF);
      file.close();
    }
    fd = open(path.c_str(), O_RDWR);
  }
  return fd;
}

static void closeSaveFileDesc()
{
  close(ndsSaveFd);
  ndsSaveFd = -1;

  close(gbaSaveFd);
  gbaSaveFd = -1;
}

static bool createCore(std::string ndsRom = "", std::string gbaRom = "")
{
  try
  {
    if (core) delete core;

    closeSaveFileDesc();

    if (ndsRom != "") ndsSaveFd = getSaveFileDesc(ndsRom);
    if (gbaRom != "") gbaSaveFd = getSaveFileDesc(gbaRom);

    core = new Core(ndsRom, gbaRom, 0, -1, -1, ndsSaveFd, gbaSaveFd);
    return true;
  }
  catch (CoreError e)
  {
    closeSaveFileDesc();

    switch (e)
    {
      case ERROR_BIOS: logCallback(RETRO_LOG_INFO, "Error Loading BIOS"); break;
      case ERROR_FIRM: logCallback(RETRO_LOG_INFO, "Error Loading Firmware"); break;
      case ERROR_ROM:  logCallback(RETRO_LOG_INFO, "Error Loading ROM"); break;
    }

    core = nullptr;
    return false;
  }
}

void retro_get_system_info(retro_system_info* info)
{
  info->need_fullpath = true;
  info->valid_extensions = "nds";
  info->library_version = VERSION;
  info->library_name = "NooDS";
  info->block_extract = false;
}

void retro_get_system_av_info(retro_system_av_info* info)
{
  info->geometry.base_width = layout.minWidth;
  info->geometry.base_height = layout.minHeight;

  info->geometry.max_width = info->geometry.base_width;
  info->geometry.max_height = info->geometry.base_height;
  info->geometry.aspect_ratio = (float)touch.minWidth / (float)touch.minHeight;

  info->timing.fps = 32.0f * 1024.0f * 1024.0f / 560190.0f;
  info->timing.sample_rate = 32.0f * 1024.0f;
}

void retro_set_environment(retro_environment_t cb)
{
  const struct retro_system_content_info_override contentOverrides[] = {
    { "nds|gba", true, false },
    {}
  };

  cb(RETRO_ENVIRONMENT_SET_CONTENT_INFO_OVERRIDE, (void*)contentOverrides);

  static const struct retro_subsystem_memory_info ndsMemory[] = {
    { "sav", RETRO_MEMORY_SAVE_RAM },
  };

  static const struct retro_subsystem_rom_info dualSlot[] = {
    { "Nintendo DS (Slot 1)", "nds", true, false, true, ndsMemory, 1 },
    { "GBA (Slot 2)", "gba", true, false, true, nullptr, 0 },
  };

  static const struct retro_subsystem_rom_info gbaSlot[] = {
    { "GBA (Slot 2)", "gba", true, false, true, ndsMemory, 1 },
  };

  const struct retro_subsystem_info subsystems[] = {
    { "Slot 1 & 2 Boot", "nds", dualSlot, 2, 1 },
    { "Slot 2 Boot", "gba", gbaSlot, 1, 2 },
    {}
  };

  cb(RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO, (void*)subsystems);

  bool nogameSupport = true;
  cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &nogameSupport);

  envCallback = cb;
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
  videoCallback = cb;
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
  audioBatchCallback = cb;
}

void retro_set_audio_sample(retro_audio_sample_t cb)
{
}

void retro_set_input_poll(retro_input_poll_t cb)
{
  inputPollCallback = cb;
}

void retro_set_input_state(retro_input_state_t cb)
{
  inputStateCallback = cb;
}

void retro_init(void)
{
  enum retro_pixel_format xrgb888 = RETRO_PIXEL_FORMAT_XRGB8888;
  envCallback(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &xrgb888);

  micInterface.interface_version = RETRO_MICROPHONE_INTERFACE_VERSION;
  micAvailable = envCallback(RETRO_ENVIRONMENT_GET_MICROPHONE_INTERFACE, &micInterface);

  if (envCallback(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging))
    logCallback = logging.log;
  else
    logCallback = logFallback;

  systemPath = normalizePath(getSystemDir(), true);
  savesPath = normalizePath(getSaveDir(), true);
}

void retro_deinit(void)
{
  logCallback = nullptr;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info* info, size_t size)
{
  ndsPath = "";
  gbaPath = "";

  for (size_t i = 0; i < size; i++)
  {
    std::string path = normalizePath(info[i].path);

    if (endsWith(path, ".nds")) ndsPath = path;
    if (endsWith(path, ".gba")) gbaPath = path;
  }

  initConfig();
  updateConfig();

  if (createCore(ndsPath, gbaPath))
  {
    gbaModeEnabled = core->gbaMode;

    updateScreenLayout();
    updateScreenState();

    initInput();
    openMicrophone();

    core->cartridgeNds.writeSave();
    core->cartridgeGba.writeSave();

    return true;
  }

  return false;
}

bool retro_load_game(const struct retro_game_info* info)
{
  size_t bootType = 0;
  size_t infoSize = info ? 1 : 0;

  return retro_load_game_special(bootType, info, infoSize);
}

void retro_unload_game(void)
{
  if (core)
  {
    core->cartridgeNds.writeSave();
    core->cartridgeGba.writeSave();

    delete core;
  }

  closeMicrophone();
  closeSaveFileDesc();
}

void retro_reset(void)
{
  createCore(ndsPath, gbaPath);
}

void retro_run(void)
{
  checkConfigVariables();
  updateScreenState();
  updateCursorState();
  inputPollCallback();

  for (int i = 0; i < sizeof(keymap) / sizeof(*keymap); ++i)
  {
    if (getButtonState(keymap[i]))
      core->input.pressKey(i);
    else
      core->input.releaseKey(i);
  }

  if (micInputMode != "Silence")
  {
    bool micPressed = getButtonState(RETRO_DEVICE_ID_JOYPAD_L2);
    bool prevStatus = micActive;

    if (micToggled != micPressed)
    {
      if (micButtonMode == "Toggle" && micPressed)
        micActive = !micActive;

      if (micButtonMode == "Hold")
        micActive = micPressed;

      micToggled = micPressed;
    }

    if (micButtonMode == "Always")
      micActive = true;

    if (prevStatus != micActive) setMicrophoneState(micActive);
    if (micActive) sendMicSamples();
  }

  if (!renderGbaScreen)
  {
    bool swapPressed = getButtonState(RETRO_DEVICE_ID_JOYPAD_R2);

    if (screenSwapped != swapPressed)
    {
      bool needSwap = ScreenLayout::screenArrangement != 3;
      bool prevSwap = swapScreens;

      if (screenSwapMode == "Toggle" && swapPressed)
        swapScreens = !swapScreens;

      if (screenSwapMode == "Hold")
        swapScreens = swapPressed;

      if (needSwap && prevSwap != swapScreens)
      {
        swapScreenPositions(layout);
        swapScreenPositions(touch);
      }

      screenSwapped = swapPressed;
      updateScreenState();
    }
  }

  if (renderBotScreen)
  {
    bool touchScreen = false;
    auto pointerX = touchX;
    auto pointerY = touchY;

    if (touchMode == "Pointer" || touchMode == "Auto")
    {
      auto posX = inputStateCallback(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_X);
      auto posY = inputStateCallback(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_Y);

      auto newX = static_cast<int>((posX + 0x7fff) / (float)(0x7fff * 2) * touch.minWidth);
      auto newY = static_cast<int>((posY + 0x7fff) / (float)(0x7fff * 2) * touch.minHeight);

      bool inScreenX = newX >= touch.botX && newX <= touch.botX + touch.botWidth;
      bool inScreenY = newY >= touch.botY && newY <= touch.botY + touch.botHeight;

      if (inScreenX && inScreenY)
      {
        touchScreen |= inputStateCallback(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT);
        touchScreen |= inputStateCallback(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_PRESSED);
      }

      if ((posX != 0 || posY != 0) && (lastMouseX != newX || lastMouseY != newY))
      {
        lastMouseX = newX;
        lastMouseY = newY;

        pointerX = touch.getTouchX(newX, newY);
        pointerY = touch.getTouchY(newX, newY);
      }
    }

    if (touchMode == "Joystick" || touchMode == "Auto")
    {
      auto speedX = (touch.botWidth / 40.0);
      auto speedY = (touch.botHeight / 40.0);

      float moveX = getAxisState(RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X);
      float moveY = getAxisState(RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y);

      touchScreen |= getButtonState(RETRO_DEVICE_ID_JOYPAD_R3);

      if (screenRotation)
      {
        std::swap(moveX, moveY);
        if (screenRotation == 1) moveX = -moveX;
        if (screenRotation == 3) moveY = -moveY;
      }

      if (moveX != 0 || moveY != 0)
      {
        pointerX += static_cast<int>((moveX / 32767) * speedX);
        pointerY += static_cast<int>((moveY / 32767) * speedY);
      }
    }

    if (cursorTimeout && (pointerX != touchX || pointerY != touchY))
    {
      cursorVisible = true;
      cursorMovedAt = std::chrono::steady_clock::now();
    }

    touchX = clampValue(pointerX, 0, layout.botWidth);
    touchY = clampValue(pointerY, 0, layout.botHeight);

    if (touchScreen)
    {
      core->input.pressScreen();
      core->spi.setTouch(touchX, touchY);
      screenTouched = true;
    }
    else if (screenTouched)
    {
      core->input.releaseScreen();
      core->spi.clearTouch();
      screenTouched = false;
    }
  }

  core->runFrame();

  renderVideo();
  renderAudio();
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
}

size_t retro_serialize_size(void)
{
  // HACK: Usually around 6MB but can vary frame to frame!
  return 1024 * 1024 * 8;
}

bool retro_serialize(void* data, size_t size)
{
  SaveState saveState(core);
  return saveState.save(data, size);
}

bool retro_unserialize(const void* data, size_t size)
{
  SaveState saveState(core);

  if (!saveState.check(data, size))
    return false;

  return saveState.load(data, size);
}

unsigned retro_get_region(void)
{
  return RETRO_REGION_NTSC;
}

unsigned retro_api_version()
{
  return RETRO_API_VERSION;
}

size_t retro_get_memory_size(unsigned id)
{
  if (id == RETRO_MEMORY_SYSTEM_RAM)
  {
    return core->dsiMode ? 0x1000000 : 0x400000;
  }
  return 0;
}

void* retro_get_memory_data(unsigned id)
{
  if (id == RETRO_MEMORY_SYSTEM_RAM)
  {
    return core->memory.getRam();
  }
  return NULL;
}

void retro_cheat_set(unsigned index, bool enabled, const char* code)
{
  ARCheat cheat;

  cheat.name = index;
  cheat.enabled = enabled;

  std::istringstream stream(code);
  std::string line;

  while (getline(stream, line) && !line.empty())
  {
    cheat.code.push_back(strtoll(&line[0], nullptr, 16));
    cheat.code.push_back(strtoll(&line[8], nullptr, 16));
  }

  core->actionReplay.cheats.push_back(cheat);
}

void retro_cheat_reset(void)
{
  core->actionReplay.cheats.clear();
}
