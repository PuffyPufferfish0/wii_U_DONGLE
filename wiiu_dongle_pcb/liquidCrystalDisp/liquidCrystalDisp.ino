#include <LiquidCrystal.h>

// PCB Pin Mapping for ESP32-C5N8R8
// RS:6, EN:7, D4:0, D5:1, D6:2, D7:3
LiquidCrystal lcd(6, 7, 0, 1, 2, 3);

// Wii U Symbol Bitmaps (5x8 pixels)
byte heart[8]   = {0b00000, 0b01010, 0b11111, 0b11111, 0b01110, 0b00100, 0b00000, 0b00000};
byte spade[8]   = {0b00100, 0b01110, 0b11111, 0b11111, 0b00100, 0b01110, 0b00000, 0b00000};
byte diamond[8] = {0b00100, 0b01110, 0b11111, 0b01110, 0b00100, 0b00000, 0b00000, 0b00000};
byte club[8]    = {0b01110, 0b01110, 0b11111, 0b11111, 0b00100, 0b01110, 0b00000, 0b00000};

void setup() {
  // 1. Wait for power rails to stabilize at boot
  delay(500); 

  // 2. Seed the randomizer using unconnected analog pin noise & clock cycles
  randomSeed(analogRead(4) + micros()); 
  
  // 3. Initialize the LCD
  lcd.begin(16, 2);
  lcd.clear();
  
  // 4. Load the custom characters into LCD memory
  lcd.createChar(0, heart);
  lcd.createChar(1, spade);
  lcd.createChar(2, diamond);
  lcd.createChar(3, club);
  
  // 5. Draw the static UI
  lcd.setCursor(0, 0);
  lcd.print("Pair GamePad:");
  lcd.setCursor(0, 1);
  
  // 6. Generate and draw the random 4-symbol sequence once
  for(int i = 0; i < 4; i++) {
    int symbol = random(0, 4); // Pick random 0-3
    lcd.write(byte(symbol));
    lcd.print(" "); 
  }
}

void loop() {
  // Intentionally empty. 
  // The code runs once at boot. To get a new code, unplug and replug the dongle!
}