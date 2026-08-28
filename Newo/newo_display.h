#pragma once

#include <Adafruit_ST7789.h>

class NewoDisplay {
 public:
  NewoDisplay();
  void begin();

 private:
  Adafruit_ST7789 display_;
  void drawTestScreen();
};
