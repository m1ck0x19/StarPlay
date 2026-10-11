/*
  Star Buddy: a tiny desk pet with a round screen, one button, and a
  Bluetooth shortcut. Adapted from the "Starbie" sketch.

  HARDWARE
    - ESP32-C3 SuperMini
    - GC9A01 round 240x240 display (SPI)
    - One push button between GPIO 3 and GND

  WHAT THE ONE BUTTON DOES
    On the pet screen:
      TAP        -> sends your keyboard shortcut over Bluetooth (play playlist)
      HOLD 0.7 s -> opens the pet menu
    In the pet menu:
      TAP        -> move the highlight to the next choice (clockwise)
      HOLD 0.7 s -> pick the highlighted choice
      (do nothing for 8 seconds and the menu closes by itself)
    On the stats screen:
      any press  -> go back

  BEFORE YOU UPLOAD (Arduino IDE)
    1. Boards Manager: install "esp32 by Espressif Systems" version 2.0.17.
       The Bluetooth keyboard library below does not compile on version 3.x.
    2. Library Manager: install "Adafruit GFX Library" and
       "Adafruit GC9A01A". Click Install All if asked for dependencies.
    3. Download https://github.com/T-vK/ESP32-BLE-Keyboard (green Code
       button, Download ZIP), then Sketch > Include Library > Add .ZIP Library.
    4. Tools menu: Board = "ESP32C3 Dev Module",
       USB CDC On Boot = "Enabled",
       Partition Scheme = "Huge APP (3MB No OTA/1MB SPIFFS)".
    5. Save this file in a folder called star_buddy, then upload.

  PAIRING
    Open your computer's Bluetooth settings and pair with "Star Buddy".
    Then give your computer a keyboard shortcut that plays your playlist,
    using the keys set in the SHORTCUT section below.
*/

#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_GC9A01A.h>
#include <BleKeyboard.h>
#include <Preferences.h>
#include <math.h>

// A helper to write colors as red, green, blue values from 0 to 255.
constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

// =========================== BEGINNER SETTINGS ===========================
// Everything most builders will want to customize is in this one section.

// --- Your board's pins ----------------------------------------------------
// ESP32-C3 GPIO numbers. These match the star PCB schematic. Pins 4 and 6
// are also the C3's built-in SPI pins, so the display library finds them itself.
const int TFT_SCK_PIN = 4;    // display SCL
const int TFT_MOSI_PIN = 6;   // display SDA
const int TFT_DC_PIN = 1;     // display DC
const int TFT_CS_PIN = 7;     // display CS
const int TFT_RST_PIN = 0;    // display RST
const int BUTTON_PIN = 3;     // button to GND

// --- Display --------------------------------------------------------------
const uint8_t DISPLAY_ROTATION = 0;       // 0, 1, 2 or 3: turns the picture 90 degrees each step
const bool COLORS_LOOK_INVERTED = false;  // set true if black and white look swapped
const uint32_t TFT_SPI_HZ = 40000000;     // lower to 20000000 if the picture glitches
const uint16_t PET_COLOR = rgb565(255, 214, 90);  // star yellow. Try rgb565(255, 255, 255) for white
const uint16_t BACKGROUND_COLOR = 0x0000;         // black

// --- Bluetooth shortcut ---------------------------------------------------
// A short tap sends these keys together. Pick a combination nothing else
// uses. Ctrl+Alt+Shift+F13 is a good choice. On a Mac, Alt is Option and
// GUI is Command.
const char *BLE_DEVICE_NAME = "Star Buddy";
const bool SHORTCUT_USES_CTRL = true;
const bool SHORTCUT_USES_ALT = true;
const bool SHORTCUT_USES_SHIFT = true;
const bool SHORTCUT_USES_GUI = false;
const uint8_t SHORTCUT_KEY = KEY_F13;     // KEY_F13 to KEY_F24 work well, or a letter like 'p'
const uint16_t SHORTCUT_HOLD_MS = 60;     // how long the keys are held down
const int SHORTCUT_JOY_CHANGE = 2;        // each shortcut makes the pet a little happier

// --- Button feel ----------------------------------------------------------
const uint16_t LONG_PRESS_MS = 700;
const uint16_t MENU_IDLE_MS = 8000;       // menu closes after this long with no presses
const uint16_t STATS_SHOW_MS = 6000;      // stats screen closes after this long
const uint16_t NOT_PAIRED_SHOW_MS = 2000; // how long the "NOT PAIRED" note stays

// --- Starting stats -------------------------------------------------------
// Stats are 0 to 100. They only change when you pick an action or press the
// shortcut; nothing slowly drains while it sits on your desk.
const int STARTING_JOY = 70;
const int STARTING_ENERGY = 75;
const int STARTING_FULLNESS = 65;

// Set true, upload once, then set it back to false if you want a fresh pet.
const bool RESET_SAVED_PET_ON_BOOT = false;

// --- Pet menu -------------------------------------------------------------
// These four actions appear at TOP, RIGHT, BOTTOM, then LEFT in the menu.
// A fifth choice, STATS, sits in the middle. The last word of each action
// controls the matching little visual reaction.
enum PetReaction {
  NAP_REACTION,
  JUMP_REACTION,
  HEART_REACTION,
  RUN_REACTION,
};

struct MenuItem {
  const char *label;
  int joyChange;
  int energyChange;
  int fullnessChange;
  PetReaction reaction;
};

const MenuItem MENU_ITEMS[] = {
  {"NAP",   1,  18, -4, NAP_REACTION},   // top: sleep and emit Zs
  {"PLAY", 12, -9, -5, RUN_REACTION},    // right: two fast laps + hearts
  {"FEED",  3,  2,  18, JUMP_REACTION},  // bottom: wiggle and jump
  {"PET",   7,  0,  0, HEART_REACTION},  // left: jump and emit hearts
};
const int MENU_ITEM_COUNT = sizeof(MENU_ITEMS) / sizeof(MENU_ITEMS[0]);

// --- Your pet's bitmap and animation -------------------------------------
// A simplified 32 by 32, one-bit version of the supplied character. You can
// replace it with your own art later; keep these size values matched to the
// sprite array you paste in.
const int PET_SPRITE_WIDTH = 32;
const int PET_SPRITE_HEIGHT = 32;
const int PET_SCALE = 2;                  // 1 = small, 2 = big (the round screen is large)

// The pet wanders unless it is asleep. A non-nap action does a tiny shake,
// then a jump, except PLAY which runs two fast laps.
const uint16_t PET_WALK_PIXEL_MS = 80;
const uint16_t PET_PRE_JUMP_MS = 230;
const uint16_t PET_JUMP_MS = 430;
const int PET_JUMP_HEIGHT = 14;
const uint32_t NAP_DURATION_MS = 48000;   // 48 seconds
const uint16_t HEARTS_DURATION_MS = 1600;
const uint16_t PLAY_LAP_MS = 800;
const uint8_t PLAY_LAP_COUNT = 2;

// Each byte stores eight pixels, left to right.
const uint8_t PROGMEM PET_SPRITE[] = {
  0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00,
  0x00, 0x70, 0x0a, 0x00,
  0x00, 0xf8, 0x1f, 0x00,
  0x00, 0x8c, 0x3f, 0x80,
  0x00, 0x43, 0xff, 0x00,
  0x00, 0x43, 0xff, 0x00,
  0x00, 0x30, 0x7e, 0x00,
  0x00, 0x98, 0x01, 0x00,
  0x01, 0x98, 0x01, 0x80,
  0x03, 0x00, 0x10, 0xc0,
  0x03, 0x04, 0x00, 0xc0,
  0x01, 0x8b, 0x01, 0x80,
  0x01, 0xcb, 0x03, 0x80,
  0x03, 0xc0, 0x03, 0xc0,
  0x03, 0xc0, 0x03, 0xc0,
  0x01, 0xc0, 0x03, 0x80,
  0x01, 0x80, 0x01, 0x80,
  0x00, 0x60, 0x06, 0x00,
  0x00, 0x3f, 0xfc, 0x00,
  0x00, 0x7f, 0xfe, 0x00,
  0x00, 0x70, 0x0e, 0x00,
  0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00,
};
// ===========================================================================
// You can read below without needing to understand every line. The rest of
// the sketch handles the button, Bluetooth, drawing, and saving automatically.
//
// How drawing works: everything is drawn on a small 120 by 120 one-bit
// canvas, then each frame is enlarged 2x onto the 240 by 240 round screen.
// The screen is round, so the corners are never visible; the layout below
// keeps everything inside a circle of radius 60 around the canvas center.

const int CANVAS_SIZE = 120;
const int SCREEN_SIZE = 240;
const int CENTER = CANVAS_SIZE / 2;
const int PET_FEET_Y = 84;        // where the pet's feet touch the "ground"
const int PET_EDGE_MARGIN = 10;   // keeps the pet's body inside the circle
const int MENU_ITEM_X[] = {60, 100, 60, 20};
const int MENU_ITEM_Y[] = {19, 60, 101, 60};
const int MENU_BOX_W = 30;
const int MENU_BOX_H = 12;
const int STATS_CHOICE = MENU_ITEM_COUNT;  // the menu cursor value that means "STATS"
const uint16_t BUTTON_DEBOUNCE_MS = 30;
const int PET_SPRITE_BYTES_PER_ROW = (PET_SPRITE_WIDTH + 7) / 8;

Adafruit_GC9A01A tft(TFT_CS_PIN, TFT_DC_PIN, TFT_RST_PIN);
GFXcanvas1 canvas(CANVAS_SIZE, CANVAS_SIZE);
BleKeyboard bleKeyboard(BLE_DEVICE_NAME, "Star Buddy", 100);
Preferences preferences;

struct PetState {
  int joy;
  int energy;
  int fullness;
};

enum ButtonEvent {
  NO_EVENT,
  SHORT_PRESS,
  LONG_PRESS,
};

struct ButtonState {
  bool stableState;
  bool lastRawState;
  bool longPressSent;
  uint32_t lastChangedAt;
  uint32_t pressedAt;
};

enum View {
  PET_VIEW,
  MENU_VIEW,
  STATS_VIEW,
};

PetState pet = {STARTING_JOY, STARTING_ENERGY, STARTING_FULLNESS};
ButtonState button = {HIGH, HIGH, false, 0, 0};
View currentView = PET_VIEW;

int menuCursor = 0;
uint32_t lastInteractionAt = 0;
uint32_t statsShownAt = 0;
uint32_t notPairedUntil = 0;
uint32_t petJumpStartedAt = 0;
uint32_t nappingUntil = 0;
uint32_t heartAnimationEndsAt = 0;
uint32_t playRunStartedAt = 0;
int nappingPetX = 0;

// The part of the sprite that actually has pixels, found automatically.
int inkLeft = 0;
int inkRight = PET_SPRITE_WIDTH - 1;
int inkTop = 0;
int inkBottom = PET_SPRITE_HEIGHT - 1;

// ------------------------------------------------------------------------
// Pet stats and saving

int keepInRange(int value, int smallest, int largest) {
  return constrain(value, smallest, largest);
}

void changePet(int joyChange, int energyChange, int fullnessChange) {
  pet.joy = keepInRange(pet.joy + joyChange, 0, 100);
  pet.energy = keepInRange(pet.energy + energyChange, 0, 100);
  pet.fullness = keepInRange(pet.fullness + fullnessChange, 0, 100);
}

void loadPet() {
  preferences.begin("starbuddy", false);
  if (RESET_SAVED_PET_ON_BOOT) {
    preferences.clear();
  }
  pet.joy = preferences.getInt("joy", STARTING_JOY);
  pet.energy = preferences.getInt("energy", STARTING_ENERGY);
  pet.fullness = preferences.getInt("full", STARTING_FULLNESS);
}

void savePet() {
  preferences.putInt("joy", pet.joy);
  preferences.putInt("energy", pet.energy);
  preferences.putInt("full", pet.fullness);
}

// ------------------------------------------------------------------------
// Sprite helpers

bool spritePixel(int column, int row) {
  const uint8_t bits = pgm_read_byte(&PET_SPRITE[row * PET_SPRITE_BYTES_PER_ROW + column / 8]);
  return (bits & (0x80 >> (column % 8))) != 0;
}

// Finds the left, right, top and bottom edges of the drawn creature, so the
// pet can be kept neatly inside the round screen whatever art you use.
void findSpriteEdges() {
  inkLeft = PET_SPRITE_WIDTH;
  inkRight = -1;
  inkTop = PET_SPRITE_HEIGHT;
  inkBottom = -1;
  for (int row = 0; row < PET_SPRITE_HEIGHT; row++) {
    for (int column = 0; column < PET_SPRITE_WIDTH; column++) {
      if (spritePixel(column, row)) {
        inkLeft = min(inkLeft, column);
        inkRight = max(inkRight, column);
        inkTop = min(inkTop, row);
        inkBottom = max(inkBottom, row);
      }
    }
  }
  if (inkRight < 0) {  // an empty sprite: fall back to the full box
    inkLeft = 0;
    inkRight = PET_SPRITE_WIDTH - 1;
    inkTop = 0;
    inkBottom = PET_SPRITE_HEIGHT - 1;
  }
}

int petMinX() {
  return PET_EDGE_MARGIN - inkLeft * PET_SCALE;
}

int petMaxX() {
  return CANVAS_SIZE - PET_EDGE_MARGIN - (inkRight + 1) * PET_SCALE;
}

int petGroundY() {
  return PET_FEET_Y - (inkBottom + 1) * PET_SCALE;
}

void drawSprite(int x, int y) {
  for (int row = 0; row < PET_SPRITE_HEIGHT; row++) {
    for (int column = 0; column < PET_SPRITE_WIDTH; column++) {
      if (spritePixel(column, row)) {
        canvas.fillRect(x + column * PET_SCALE, y + row * PET_SCALE, PET_SCALE, PET_SCALE, 1);
      }
    }
  }
}

// ------------------------------------------------------------------------
// Button

// Returns SHORT_PRESS when the button is released quickly, and LONG_PRESS as
// soon as it has been held long enough (without waiting for release).
ButtonEvent readButton() {
  const bool rawState = digitalRead(BUTTON_PIN);
  const uint32_t now = millis();

  if (rawState != button.lastRawState) {
    button.lastRawState = rawState;
    button.lastChangedAt = now;
  }

  if (rawState != button.stableState && now - button.lastChangedAt >= BUTTON_DEBOUNCE_MS) {
    button.stableState = rawState;
    if (button.stableState == LOW) {
      button.pressedAt = now;
      button.longPressSent = false;
    } else if (!button.longPressSent) {
      return SHORT_PRESS;
    }
  }

  if (button.stableState == LOW && !button.longPressSent &&
      now - button.pressedAt >= LONG_PRESS_MS) {
    button.longPressSent = true;
    return LONG_PRESS;
  }
  return NO_EVENT;
}

// ------------------------------------------------------------------------
// Pet movement

int petWalkingX(uint32_t now) {
  const int farthest = petMaxX() - petMinX();
  const uint32_t roundTrip = static_cast<uint32_t>(farthest) * 2;
  const uint32_t step = (now / PET_WALK_PIXEL_MS) % roundTrip;
  return petMinX() + (step <= static_cast<uint32_t>(farthest) ? step : roundTrip - step);
}

uint32_t playRunDuration() {
  return static_cast<uint32_t>(PLAY_LAP_MS) * PLAY_LAP_COUNT;
}

bool isNapping() {
  return nappingUntil != 0 && millis() < nappingUntil;
}

bool isPlaying() {
  return playRunStartedAt != 0 && millis() - playRunStartedAt < playRunDuration();
}

int playRunX(uint32_t now) {
  const int farthest = petMaxX() - petMinX();
  const uint32_t lapAge = (now - playRunStartedAt) % PLAY_LAP_MS;
  const float progress = static_cast<float>(lapAge) / PLAY_LAP_MS;
  const float along = progress < 0.5f ? progress * 2.0f : (1.0f - progress) * 2.0f;
  return petMinX() + static_cast<int>(along * farthest);
}

void updatePetTimers() {
  if (nappingUntil != 0 && !isNapping()) {
    nappingUntil = 0;  // The pet wakes naturally when its nap time is over.
  }
  if (playRunStartedAt != 0 && !isPlaying()) {
    playRunStartedAt = 0;  // PLAY is exactly two laps.
  }
}

void startReaction(PetReaction reaction) {
  const uint32_t now = millis();
  nappingUntil = 0;
  heartAnimationEndsAt = 0;
  playRunStartedAt = 0;

  if (reaction == NAP_REACTION) {
    petJumpStartedAt = 0;
    nappingPetX = petWalkingX(now);
    nappingUntil = now + NAP_DURATION_MS;
  } else if (reaction == RUN_REACTION) {
    petJumpStartedAt = 0;
    playRunStartedAt = now;
    heartAnimationEndsAt = now + playRunDuration();
  } else {
    // A confirmed action gets a little anticipation shake before the jump.
    petJumpStartedAt = now;
    if (reaction == HEART_REACTION) {
      heartAnimationEndsAt = now + HEARTS_DURATION_MS;
    }
  }
}

// ------------------------------------------------------------------------
// Actions

// Sends the keyboard shortcut to the paired computer.
void sendShortcut() {
  if (!bleKeyboard.isConnected()) {
    notPairedUntil = millis() + NOT_PAIRED_SHOW_MS;
    return;
  }

  if (SHORTCUT_USES_CTRL) bleKeyboard.press(KEY_LEFT_CTRL);
  if (SHORTCUT_USES_ALT) bleKeyboard.press(KEY_LEFT_ALT);
  if (SHORTCUT_USES_SHIFT) bleKeyboard.press(KEY_LEFT_SHIFT);
  if (SHORTCUT_USES_GUI) bleKeyboard.press(KEY_LEFT_GUI);
  bleKeyboard.press(SHORTCUT_KEY);
  delay(SHORTCUT_HOLD_MS);
  bleKeyboard.releaseAll();

  changePet(SHORTCUT_JOY_CHANGE, 0, 0);
  savePet();
  startReaction(HEART_REACTION);  // the pet celebrates the music
}

void openMenu() {
  currentView = MENU_VIEW;
  menuCursor = 0;
}

void confirmMenuChoice() {
  if (menuCursor == STATS_CHOICE) {
    currentView = STATS_VIEW;
    statsShownAt = millis();
    return;
  }

  const MenuItem &item = MENU_ITEMS[menuCursor];
  changePet(item.joyChange, item.energyChange, item.fullnessChange);
  savePet();
  startReaction(item.reaction);
  currentView = PET_VIEW;
}

void handleButton() {
  const ButtonEvent event = readButton();
  if (event == NO_EVENT) {
    return;
  }
  lastInteractionAt = millis();

  if (currentView == PET_VIEW) {
    if (event == SHORT_PRESS) {
      sendShortcut();
    } else {
      openMenu();
    }
  } else if (currentView == MENU_VIEW) {
    if (event == SHORT_PRESS) {
      menuCursor = (menuCursor + 1) % (MENU_ITEM_COUNT + 1);
    } else {
      confirmMenuChoice();
    }
  } else {
    currentView = PET_VIEW;  // any press leaves the stats screen
  }
}

void handleTimeouts() {
  const uint32_t now = millis();
  if (currentView == MENU_VIEW && now - lastInteractionAt >= MENU_IDLE_MS) {
    currentView = PET_VIEW;
  }
  if (currentView == STATS_VIEW && now - statsShownAt >= STATS_SHOW_MS) {
    currentView = PET_VIEW;
  }
}

// ------------------------------------------------------------------------
// Drawing

void drawHeart(int x, int y) {
  // A tiny seven-pixel-wide heart that stays crisp at any size.
  canvas.fillRect(x - 2, y, 2, 2, 1);
  canvas.fillRect(x + 1, y, 2, 2, 1);
  canvas.fillRect(x - 3, y + 2, 7, 2, 1);
  canvas.fillRect(x - 2, y + 4, 5, 1, 1);
  canvas.fillRect(x - 1, y + 5, 3, 1, 1);
  canvas.drawPixel(x, y + 6, 1);
}

void drawHearts(uint32_t now, int centerX, int topY) {
  if (now >= heartAnimationEndsAt) {
    return;
  }

  const float progress =
      1.0f - static_cast<float>(heartAnimationEndsAt - now) / HEARTS_DURATION_MS;
  const int rise = static_cast<int>(progress * 16.0f);
  drawHeart(constrain(centerX - 7, 16, 104), topY - 8 - rise);
  drawHeart(constrain(centerX + 7, 16, 104), topY - 14 - rise / 2);
}

void drawSleepZs(uint32_t now, int rightX, int topY) {
  // The Zs float upward, then loop back down while the pet is napping.
  const int rise = static_cast<int>((now / 300UL) % 12UL);
  const int zx = constrain(rightX - 8, 14, 98);
  canvas.setTextSize(1);
  canvas.setTextColor(1);
  canvas.setCursor(zx, topY - 8 - rise);
  canvas.print("z");
  canvas.setCursor(zx + 6, topY - 14 - rise / 2);
  canvas.print("z");
}

// A small Bluetooth label at the top. A line through it means "not connected".
void drawBluetoothStatus() {
  canvas.setTextSize(1);
  canvas.setTextColor(1);
  canvas.setCursor(54, 8);
  canvas.print("BT");
  if (!bleKeyboard.isConnected()) {
    canvas.drawLine(52, 17, 68, 6, 1);
  }
}

void drawPet() {
  const uint32_t now = millis();
  int petX = isNapping() ? nappingPetX : petWalkingX(now);
  if (isPlaying()) {
    petX = playRunX(now);
  }
  int petY = petGroundY();

  // A confirmed action starts with a quick shake, then a jump.
  if (!isNapping() && petJumpStartedAt != 0) {
    const uint32_t animationAge = now - petJumpStartedAt;

    if (animationAge < PET_PRE_JUMP_MS) {
      petX += static_cast<int>(sinf(now / 16.0f) * 2.0f);
    } else if (animationAge < PET_PRE_JUMP_MS + PET_JUMP_MS) {
      const float jumpProgress =
          static_cast<float>(animationAge - PET_PRE_JUMP_MS) / PET_JUMP_MS;
      petY -= static_cast<int>(sinf(jumpProgress * PI) * PET_JUMP_HEIGHT);
    } else {
      petJumpStartedAt = 0;
    }
  }

  petX = constrain(petX, petMinX(), petMaxX());
  drawSprite(petX, petY);

  const int bodyTopY = petY + inkTop * PET_SCALE;
  const int bodyRightX = petX + (inkRight + 1) * PET_SCALE;
  const int bodyCenterX = petX + (inkLeft + inkRight + 1) * PET_SCALE / 2;

  if (isNapping()) {
    drawSleepZs(now, bodyRightX, bodyTopY);
  }
  drawHearts(now, bodyCenterX, bodyTopY);
  drawBluetoothStatus();

  if (millis() < notPairedUntil) {
    canvas.setTextSize(1);
    canvas.setTextColor(1);
    canvas.setCursor(30, 26);
    canvas.print("NOT PAIRED");
  }
}

void drawMenuItem(int item) {
  const int boxX = MENU_ITEM_X[item] - MENU_BOX_W / 2;
  const int boxY = MENU_ITEM_Y[item] - MENU_BOX_H / 2;
  const bool isSelected = item == menuCursor;

  if (isSelected) {
    canvas.fillRoundRect(boxX, boxY, MENU_BOX_W, MENU_BOX_H, 3, 1);
    canvas.setTextColor(0);
  } else {
    canvas.drawRoundRect(boxX, boxY, MENU_BOX_W, MENU_BOX_H, 3, 1);
    canvas.setTextColor(1);
  }

  canvas.setTextSize(1);
  const int labelLength = strlen(MENU_ITEMS[item].label);
  canvas.setCursor(MENU_ITEM_X[item] - labelLength * 3, MENU_ITEM_Y[item] - 3);
  canvas.print(MENU_ITEMS[item].label);
  canvas.setTextColor(1);
}

void drawMenu() {
  canvas.drawCircle(CENTER, CENTER, 58, 1);
  for (int item = 0; item < MENU_ITEM_COUNT; item++) {
    drawMenuItem(item);
  }

  canvas.setTextSize(1);
  if (menuCursor == STATS_CHOICE) {
    canvas.fillRoundRect(38, 50, 44, 20, 4, 1);
    canvas.setTextColor(0);
    canvas.setCursor(45, 56);
    canvas.print("STATS");
    canvas.setTextColor(1);
  } else {
    canvas.setTextColor(1);
    canvas.setCursor(36, 50);
    canvas.print("TAP:NEXT");
    canvas.setCursor(39, 62);
    canvas.print("HOLD:OK");
  }
}

void drawStatBar(int y, const char *label, int value) {
  canvas.setTextSize(1);
  canvas.setTextColor(1);
  canvas.setCursor(12, y);
  canvas.print(label);
  canvas.drawRect(50, y, 38, 8, 1);
  canvas.fillRect(51, y + 1, map(value, 0, 100, 0, 36), 6, 1);
  canvas.setCursor(92, y);
  canvas.print(value);
}

void drawStats() {
  canvas.setTextSize(1);
  canvas.setTextColor(1);
  canvas.setCursor(45, 18);
  canvas.print("PET STATS");
  drawStatBar(38, "JOY", pet.joy);
  drawStatBar(56, "ENERGY", pet.energy);
  drawStatBar(74, "FULL", pet.fullness);
}

// Enlarges the 120 by 120 canvas 2x and sends it to the round screen.
void pushFrameToScreen() {
  static uint16_t twoRows[SCREEN_SIZE * 2];
  const uint8_t *bits = canvas.getBuffer();
  const int bytesPerRow = (CANVAS_SIZE + 7) / 8;

  for (int y = 0; y < CANVAS_SIZE; y++) {
    for (int x = 0; x < CANVAS_SIZE; x++) {
      const bool on = (bits[y * bytesPerRow + x / 8] & (0x80 >> (x % 8))) != 0;
      const uint16_t color = on ? PET_COLOR : BACKGROUND_COLOR;
      twoRows[x * 2] = color;
      twoRows[x * 2 + 1] = color;
    }
    memcpy(&twoRows[SCREEN_SIZE], twoRows, SCREEN_SIZE * sizeof(uint16_t));
    tft.drawRGBBitmap(0, y * 2, twoRows, SCREEN_SIZE, 2);
  }
}

void drawCurrentView() {
  canvas.fillScreen(0);
  if (currentView == MENU_VIEW) {
    drawMenu();
  } else if (currentView == STATS_VIEW) {
    drawStats();
  } else {
    drawPet();
  }
  pushFrameToScreen();
}

// ------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  button.stableState = digitalRead(BUTTON_PIN);
  button.lastRawState = button.stableState;
  loadPet();
  findSpriteEdges();

  SPI.begin(TFT_SCK_PIN, -1, TFT_MOSI_PIN, TFT_CS_PIN);
  tft.begin(TFT_SPI_HZ);
  tft.setRotation(DISPLAY_ROTATION);
  if (COLORS_LOOK_INVERTED) {
    tft.invertDisplay(false);  // the driver turns inversion on by default
  }
  tft.fillScreen(BACKGROUND_COLOR);

  bleKeyboard.begin();  // starts advertising as BLE_DEVICE_NAME
  lastInteractionAt = millis();
  drawCurrentView();
}

void loop() {
  updatePetTimers();
  handleButton();
  handleTimeouts();
  drawCurrentView();
  delay(10);
}
