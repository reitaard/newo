#include "newo_display.h"

#include <SPI.h>

#include "newo_config.h"

namespace {
constexpr uint16_t DISPLAY_WIDTH = 240;
constexpr uint16_t DISPLAY_HEIGHT = 240;
}  // namespace

NewoDisplay::NewoDisplay()
    : display_(NewoConfig::DISPLAY_CS_PIN, NewoConfig::DISPLAY_DC_PIN,
               NewoConfig::DISPLAY_RST_PIN) {}

void NewoDisplay::begin() {
  SPI.begin(NewoConfig::DISPLAY_SCK_PIN, -1, NewoConfig::DISPLAY_MOSI_PIN,
            NewoConfig::DISPLAY_CS_PIN);
  display_.init(DISPLAY_WIDTH, DISPLAY_HEIGHT);
  display_.setRotation(0);
  drawTestScreen();
}

void NewoDisplay::drawTestScreen() {
  display_.fillScreen(ST77XX_BLACK);
  display_.setTextWrap(false);
  display_.setTextColor(ST77XX_WHITE);

  display_.setTextSize(4);
  display_.setCursor(72, 78);
  display_.print("NEWO");

  display_.setTextSize(2);
  display_.setCursor(60, 140);
  display_.print("DISPLAY OK");
}
