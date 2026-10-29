#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SD.h>
#include <FS.h>
#include "AudioTools.h"

/*
This is for specifically the ESP32-C3 (supermini) and is for the official wavplayer.

Unless specified otherwise all of the constants can be changed in terms of pin assignments.
Just make sure to know the attributes of the pin you changing to, as some are different that others in terms of how boot is handled 
*/

//sleepy
#define SLEEP_INT 60000 //ms
int start_sleep = millis();
#define DSLEEP_INT 120000 //ms
int start_dsleep = millis();
unsigned long start = millis();

//pcm5102a
#define I2S_NUM         I2S_NUM_0
#define I2S_BCK_IO      1
#define I2S_LRCK_IO     3
#define I2S_DATA_IO     2

//please remember to change these for your personal configuration if you decide to change any of this
//#define UP 16
//#define DOWN 17
//#define SELECT 18
//#define BACK 4

#define UP 2675
#define DOWN 4010
#define SELECT 3350
#define BACK 9999 //disable
#define WAKE GPIO_NUM_0 //idk if this will work
#define BUTTONS 0 //analog pin, needs to be of 1_ 
#define SENSITIVITY 50 //the range pos and neg that is allowable via the analog pin
//sd card
#define SCK 4
#define MISO 5 //if customizing, do NOT use pin 12. A basic SD module pulls it high preventing boot. Horseshit.
#define MOSI 6
#define CS 7

//font
#define CHAR_SIZE 1
#define CHAR_W (6 * CHAR_SIZE)
#define CHAR_H (8 * CHAR_SIZE)
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

#define ARR_SIZE 10 //menu max size
#define MAX_CHAR 19 //max amount of chars that can fit on the screen -1 for song progress bar
#define MAX_FILES 512 //files to be listed in folder
#define MAX_DIR 64 //max listed folders

#define OLED_RESET -1
#define VISIBLE_LINES (SCREEN_HEIGHT / CHAR_H)

int scroll_offset = 0;

char song_folders[MAX_DIR][MAX_CHAR] = {0}; //2
char songs[MAX_FILES][MAX_CHAR] = {0}; //3
char current_dir[MAX_CHAR] = {0};
char playing[MAX_CHAR] = {0};
int menu_index = 0;
int current_menu = 0;
int previous_menu = 0;

I2SStream i2s;
EncodedAudioStream dec(&i2s, new WAVDecoder());
StreamCopy *copier;
AudioInfo info(44100, 2, 16);
File current_song;

long total_bytes;
float recently_updated_value;

int current_brightness = 2;
uint8_t brightness_value = 0x7F;

SPIClass spi = SPIClass(FSPI);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
struct MenuCall {
  const char *name;
  void (*call)(void);
};

struct Menus {
  int index;
  char (*array)[MAX_CHAR];
};

char menu[ARR_SIZE][MAX_CHAR] = { //0
  "Current Song",
  "Play Random",
  "Song Selection",
  "Settings",
  "Poweroff",
  "",
  ""
};


char settings_menu[ARR_SIZE][MAX_CHAR] = { //1
  "Brightness",
  "Create Folders",
  "Sleep Interval",
  "Check Battery",
  "Diagnostics",
  "Unmount Card",
  ""
};

//control
int pause_song = 0;

void print(const char *text, int x, int y, int clr = 0) {
  if (clr) display.clearDisplay();
  display.setCursor(x * CHAR_W, y * CHAR_H);
  display.println(text);
}

void print(int value, int x, int y, int clr = 0) {
  if (clr) display.clearDisplay();

  display.setCursor(x * CHAR_W, y * CHAR_H);
  display.println(value);
}

void print(float value, int x, int y, int clr = 0) {
  if (clr) display.clearDisplay();

  display.setCursor(x * CHAR_W, y * CHAR_H);
  display.println(value);
}

/*
MAIN
*/

void setup() {
  Serial.begin(115200);
  analogReadResolution(12);
  start_sleep = millis();
  start_dsleep = millis();

  auto config = i2s.defaultConfig(TX_MODE);   
  config.copyFrom(info); 
  config.pin_bck = I2S_BCK_IO;
  config.pin_ws = I2S_LRCK_IO;
  config.pin_data = I2S_DATA_IO;
  Serial.println("I2S");
  i2s.begin(config);
  Serial.println("DEC");
  dec.begin();
  Serial.println("DISPLAY");
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  display.ssd1306_command(SSD1306_SETCONTRAST);
  display.ssd1306_command(brightness_value);
  display.setTextSize(1);
  display.setTextColor(WHITE);
  print("Checking SD card module..", 0, 0, 1);
  display.display();
  delay(500);
  Serial.println("SPI");
  spi.begin(SCK, MISO, MOSI, CS);
  Serial.println("SD");
  if (!SD.begin(CS, spi, 1000000)) {
    print("Mount Failed", 0, 0, 1);
    strcpy(menu[5], "Remount Card");
    display.display();
  }
  uint8_t cardType = SD.cardType();

  if(cardType == CARD_NONE){
    print("No SD card attached", 0, 0, 1);
    display.display();
    delay(2000);
  }
  Serial.println("SD OK");
  display.clearDisplay();
  ls("/", 2);
  Serial.println("LS OK");
  //stop before reaching to display errors
  draw_menu();
  Serial.println("MENU OK");
  pinMode(UP, INPUT_PULLDOWN);
  pinMode(DOWN, INPUT_PULLDOWN);
  pinMode(SELECT, INPUT_PULLDOWN);
}

void loop() {
  if (millis() - start >= 100) {
    int inp = get_input();
    exec(inp);
    start = millis();
  } else if (millis() - start_sleep >= SLEEP_INT) {
    sleepy(1);
  }
  push_song();
}

/*
END OF MAIN
*/

//input related
/*int get_input() {
  if (digitalRead(UP) == HIGH) {
    return 1;
  } else if (digitalRead(DOWN) == HIGH) {
    return -1;
  } else if (digitalRead(SELECT) == HIGH) {
    return 3;
  } else if (digitalRead(BACK) == HIGH) {
    return 2;
  }
  return 0;
}*/

int get_input() {
  int value = analogRead(BUTTONS);
  if (value > UP - SENSITIVITY && value < UP + SENSITIVITY) {
    return 1;
  } else if (value > DOWN - SENSITIVITY && value < DOWN + SENSITIVITY) {
    return -1;
  } else if (value > SELECT - SENSITIVITY && value < SELECT + SENSITIVITY) {
    return 3;
  } else if (value > BACK - SENSITIVITY && value < BACK + SENSITIVITY) {
    return 2;
  }
  return 0;
}

void back() {
  if (current_menu == previous_menu) {
    current_menu = 0;
    previous_menu = 0;
  } else {
    current_menu = previous_menu;
  }
  menu_index = 0;
  scroll_offset = 0;
}

void wait() {
  while (get_input() == 0) {
    push_song();
  }
  delay_noblock(100);
}

void delay_noblock(int wait) {
  unsigned long delayed = millis();
  while (millis() - delayed < wait) {
    push_song();
  }
}
//song related
int draw_progress(int old_width) {
  int width = ((uint64_t)current_song.position() * SCREEN_WIDTH) / total_bytes;
  if (width <= old_width) {
    return old_width;
  }
  display.fillRect(0, SCREEN_HEIGHT - 4, SCREEN_WIDTH, 4, BLACK);
  display.fillRect(0, SCREEN_HEIGHT - 4, width, 4, WHITE);
  display.display();
  return width;
}

void check_song() {
  if (!current_song.available()) {
    strcpy(playing, "None");
    print(playing, 0, 0, 1);
    display.display();
    wait();
    start_sleep = millis();
    return;
  }
  print(playing, 0, 0, 1);
  display.display();
  if (pause_song) {
    draw_pause();
  }
  int choice = 0;
  int current_width = 0;
  while (choice != 2) {
    if (millis() - start >= 150) {
      choice = get_input();
      if (choice) {
        if (choice == 3) {
          pause_song = !pause_song;
          draw_pause();
        }
        start_sleep = millis();
      }
      start = millis();
      if (!current_song) {
        check_song();
        return;
      }
    } else if (millis() - start_sleep >= SLEEP_INT) {
      sleepy(0);
      print(playing, 0, 0, 1);
      if (pause_song) {
        draw_pause();
      }
      display.display();
    }
    current_width = draw_progress(current_width);
    push_song();
  }
  start_dsleep = millis();
}

void push_song() {
  if (current_song.available() && !pause_song) {
    copier->copy();
    if (current_song.position() >= total_bytes) {
      stop_song();
    }
  }
}

void play(char *path) {
  if (current_song || copier) {
    stop_song();
  }
  current_song = SD.open(path);
  if (!current_song) {
    print("Open failed", 0, 0, 1);
    display.display();
    wait();
    return;
  }
  total_bytes = current_song.size();

  copier = new StreamCopy(dec, current_song);
  copier->begin();
  pause_song = 0;
  delay(100);
}

void stop_song() {
  pause_song = 0;
    if (copier) {
        delete copier;
        copier = nullptr;
    }
    if (current_song) {
        current_song.flush();
        current_song.close();
    }
}

void select_songs() {
  previous_menu = current_menu;
  current_menu = 2;
  menu_index = 0;
  scroll_offset = 0;
  draw_menu();
}
// Utility
void draw_pause() {
  const int bar_w = 8;
  const int bar_h = SCREEN_HEIGHT / 3;

  const int x = SCREEN_WIDTH / 2;
  const int y = (SCREEN_HEIGHT - bar_h) / 2;

  const int gap = 4;

  int x1 = x - bar_w - (gap / 2);
  int x2 = x + (gap / 2);

  uint16_t color = pause_song ? WHITE : BLACK;

  display.fillRect(x1, y, bar_w, bar_h, color);
  display.fillRect(x2, y, bar_w, bar_h, color);

  display.display();
}

void sleepy(int full_sleep) {
  start_dsleep = millis();
  display.clearDisplay();
  display.display();
  while (1) {
    if (millis() - start >= 500) {
      if (get_input()) {
        break;
      }
    start = millis();
    } else if (millis() - start_dsleep >= DSLEEP_INT && full_sleep) {
      poweroff();
    }
    push_song();
  }
  if (full_sleep) {
    draw_menu();
  }
}

void update_time() {
  if (current_song.available()) {
    display.setCursor(0, SCREEN_HEIGHT - CHAR_H);
    display.setTextColor(BLACK);
    display.printf("%.2f", recently_updated_value);
    recently_updated_value = ((float)current_song.position() / (float)total_bytes) * 100.0f;
    display.setTextColor(WHITE);
    display.printf("%.2f", recently_updated_value);
    display.display();
  } else {
    if (recently_updated_value) {
      recently_updated_value = 0.0f;
    }
  }
}

void umount() {
  stop_song();
  delay(100);
  memset(songs, 0, sizeof(songs));
  memset(song_folders, 0, sizeof(song_folders));
  strcpy(menu[5], "Remount Card");
  SD.end();
  print("Card unmounted", 0, 0, 1);
  display.display();
  wait();
}
void remount() {
  Serial.println("[SD] Attempting remount...");

  SD.end();
  delay(100);

  pinMode(CS, OUTPUT);
  digitalWrite(CS, HIGH);

  if (!SD.begin(CS, spi, 1000000)) {
    Serial.println("[SD] Mount failed");
    Serial.printf("[SD] CS pin: %d\n", CS);

    print("Mount Failed", 0, 0, 1);
    display.display();
    wait();
    return;
  }

  Serial.println("[SD] Mount successful");
}
void poweroff(){
  esp_deep_sleep_enable_gpio_wakeup(BIT(WAKE), ESP_GPIO_WAKEUP_GPIO_HIGH);
  display.clearDisplay();
  display.display();
  esp_deep_sleep_start();
}

int adjust(int pos) {
  return pos * CHAR_H;
}


//user set settings
void settings() {
  previous_menu = current_menu;
  current_menu = 1;
  menu_index = 0;
  scroll_offset = 0;
  
}

void set_brightness() {
  delay_noblock(100);

  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("Set Brightness");
  display.setCursor((SCREEN_WIDTH / 2) - 1, CHAR_H * 5);
  display.printf("%d", current_brightness);
  display.setCursor(0, CHAR_H * 3);
  display.println("UP to increase");
  display.setCursor(0, CHAR_H * 7);
  display.println("DOWN to decrease");
  display.display();
  uint8_t illumination;
  while (1) {
    if (millis() - start >= 200) {

      int inp = get_input();

      if (inp == 3) {
        break;
      }

      if (inp == 1 || inp == -1) {
        current_brightness += inp;
        if (current_brightness > 3) {
          current_brightness = 3;
        } else if (current_brightness < 0) {
          current_brightness = 0;
        }
        display.clearDisplay();
        display.setCursor(0, 0);
        display.println("Set Brightness");
        display.setCursor((SCREEN_WIDTH / 2) - 1, CHAR_H * 5);
        display.printf("%d", current_brightness);
        display.setCursor(0, CHAR_H * 3);
        display.println("UP to increase");
        display.setCursor(0, CHAR_H * 7);
        display.println("DOWN to decrease");
        display.display();
      }

      start = millis();
    }
  }
  if (current_brightness == 3) {
    brightness_value = 0xFE;

  } else if (current_brightness == 2) {
    brightness_value = 0x7E;
  } else {
    brightness_value = 0x01;
  }
  display.ssd1306_command(SSD1306_SETCONTRAST);
  display.ssd1306_command(brightness_value);
}

void set_sleep(){

}

//system info
void battery_info(){}

void diag() {
  print("Memory Stats:", 0 ,0 ,1);
  display.setCursor(0, adjust(2));
  display.printf("Total:%dkb", (ESP.getHeapSize() / 1024));
  display.setCursor(0, adjust(4));
  display.printf("Free:%dkb", (ESP.getFreeHeap() / 1024));
  display.setCursor(0, adjust(5));
  display.printf("Aval:%dkb", (ESP.getMaxAllocHeap() / 1024));
  display.display();
  delay_noblock(1000);
  wait();
}

//SD card management
int add_folder(const char *name) {
  int i = 0;
  for (i; song_folders[i][0] != '\0' && i < MAX_DIR; i++) {
    if (!strcmp(name, song_folders[i])) {
      return 0;
    }
  }
  if (i > MAX_DIR) {
    display.clearDisplay();
    print("Maximum directories reached.", 0, 0, 0);
    print("Directories allowed: 128", 0, 1, 0);
    display.display();
    wait();
    return 0;
  }
  strcpy(song_folders[i], name);
  return 1;
}

int add_file(const char *name) {
  int i = 0;
  for (i; songs[i][0] != '\0' && i < MAX_FILES; i++){
    if (!strcmp(name, songs[i])) {
      return 0;
    }
  }
  if (i > MAX_FILES) {
    display.clearDisplay();
    print("Maximum files reached.", 0, 0, 0);
    print("files allowed per folder: 1024", 0, 1, 0);
    display.display();
    wait();
    return 0;
  }
  strcpy(songs[i], name);
  return 1;
}

void ls(const char *name, int menu_type){

  File root = SD.open(name);
  if(!root){
    display.println("Failed to open card");
    return;
  }
  if(!root.isDirectory()){
    display.println("Won't open non-dir");
    return;
  }

  File file = root.openNextFile();
  while (file){
    if (file.isDirectory()){
      if (menu_type == 2) {
        add_folder(file.name());
      }
    } else {
      if (menu_type == 3) {
        add_file(file.name());
      }
    }
    file = root.openNextFile();
  }
}

void format(){
  print("Defaulting folders", 0, 0, 1);
  print("please make sure you", 0, 1, 0);
  print("have formatted this", 0, 2, 0);
  print("card in FAT32", 0, 3, 0);
  print("Otherwise, poweroff", 0, 4, 0);
  print("Press to cont.", 0, 6, 0);
  display.display();
  delay_noblock(200);
  wait();

  if (SD.mkdir("/Rock")) {
    print("Created:", 0, 0, 0);
    print("Rock",0,1, 0);
    display.display();
    delay_noblock(500);
    display.clearDisplay();
  } else {
    print("Failed to make:", 0, 0, 0);
    print("Rock",0,1, 0);
    display.display();
    delay_noblock(500);
    display.clearDisplay();
  }
  if (SD.mkdir("/Country")) {
    print("Created:", 0, 0, 0);
    print("Country",0,1, 0);
    display.display();
    delay_noblock(500);
    display.clearDisplay();
  } else {
    print("Failed to make:", 0, 0, 0);
    print("Country",0,1, 0);
    display.display();
    delay_noblock(500);
    display.clearDisplay();
  }
  if (SD.mkdir("/HipHop")) {
    print("Created:", 0, 0, 0);
    print("Hip Hop",0,1, 0);
    display.display();
    delay_noblock(500);
    display.clearDisplay();
  } else {
    print("Failed to make:", 0, 0, 0);
    print("Hip Hop",0,1, 0);
    display.display();
    delay_noblock(500);
    display.clearDisplay();
  }
  if (SD.mkdir("/Lo-Fi")) {
    print("Created:", 0, 0, 0);
    print("Lo-Fi",0,1, 0);
    display.display();
    delay_noblock(500);
    display.clearDisplay();
  } else {
    print("Failed to make:", 0, 0, 0);
    print("Lo-Fi",0,1, 0);
    display.display();
    delay_noblock(500);
    display.clearDisplay();
  }
  if (SD.mkdir("/RnB")) {
    print("Created:", 0, 0, 0);
    print("RnB",0,1, 0);
    display.display();
    delay_noblock(500);
    display.clearDisplay();
  } else {
    print("Failed to make:", 0, 0, 0);
    print("RnB",0,1, 0);
    display.display();
    delay_noblock(500);
    display.clearDisplay();
  }
  if (SD.mkdir("/Pop")) {
    print("Created:", 0, 0, 0);
    print("Pop",0,1, 0);
    display.display();
    delay_noblock(500);
    display.clearDisplay();
  } else {
    print("Failed to make:", 0, 0, 0);
    print("Pop",0,1, 0);
    display.display();
    delay_noblock(500);
    display.clearDisplay();
  }
  if (SD.mkdir("/Classical")) {
    print("Created:", 0, 0, 0);
    print("Classical",0,1, 0);
    display.display();
    delay_noblock(500);
    display.clearDisplay();
  } else {
    print("Failed to make:", 0, 0, 0);
    print("Classical",0,1, 0);
    display.display();
    delay_noblock(500);
    display.clearDisplay();
  }
  if (SD.mkdir("/Misc")) {
    print("Created:", 0, 0, 0);
    print("Misc",0,1, 0);
    display.display();
    delay_noblock(500);
    display.clearDisplay();
  } else {
    print("Failed to make:", 0, 0, 0);
    print("Misc",0,1, 0);
    display.display();
    delay_noblock(500);
    display.clearDisplay();
  }
  if (SD.mkdir("/Audio Books")) {
    print("Created:", 0, 0, 0);
    print("Audio Books",0,1, 0);
    display.display();
    delay_noblock(500);
    display.clearDisplay();
  } else {
    print("Failed to make:", 0, 0, 0);
    print("Audio Books",0,1, 0);
    display.display();
    delay_noblock(500);
    display.clearDisplay();
  }
}

//menu assignments
struct Menus menus[] {
  {0, menu},
  {1, settings_menu},
  {2, song_folders},
  {3, songs}
};

struct MenuCall Funcs[] = {
  {"Current Song", check_song},
  {"Play Random", play_random},
  {"Song Selection", select_songs},
  {"Settings", settings},
  {"Remount Card", remount},
  {"Unmount Card", umount},
  {"Poweroff", poweroff},
  {"Brightness", set_brightness},
  {"Sleep Interval", set_sleep},
  {"Check Battery", battery_info},
  {"Create Folders", format},
  {"Diagnostics", diag}
};

//calls other above functions
void play_random() {
  int randint;
  current_menu = 3;
  while (!arrlen()) {
    randint = random(0, MAX_DIR);
    Serial.println(randint);
    strcpy(current_dir, "/");
    strcat(current_dir, menus[2].array[randint]);
    memset(songs, 0, sizeof(songs));
    ls(current_dir, 3);
  }
  char full_path[256];
  randint = random(0, arrlen());
  Serial.println(randint);
  strcpy(full_path, current_dir);
  strcat(full_path, "/");
  strcat(full_path, menus[current_menu].array[randint]);
  strcpy(playing, menus[current_menu].array[randint]);
  play(full_path);
  check_song();
  current_menu = 0;
  draw_menu();
  delay_noblock(250);
}

void exec(int choice) {
  if (!choice) {
    return;
  }
  start_sleep = millis();
  if (choice == 2) {
    back();
    draw_menu();
  }
  int array_size = arrlen();

  if (choice && choice < 2) {
    menu_index += choice;

    if (menu_index < 0) {
      menu_index = 0;
    } else if (menu_index >= array_size) {
      menu_index = array_size - 1;
    }

    if (menu_index < scroll_offset) {
      scroll_offset = menu_index;
    }

    if (menu_index >= scroll_offset + VISIBLE_LINES) {
      scroll_offset = menu_index - VISIBLE_LINES + 1;
    }

    draw_menu();
  } else if (choice == 3) {
    if (current_menu == 2) {
      strcpy(current_dir, "/");
      strcat(current_dir, menus[current_menu].array[menu_index]);
      memset(songs, 0, sizeof(songs));
      ls(current_dir, 3);
      previous_menu = current_menu;
      current_menu = 3;
      menu_index = 0;
      scroll_offset = 0;
      draw_menu();
    } else if (current_menu == 3) {
      char full_path[256];
      strcpy(full_path, current_dir);
      strcat(full_path, "/");
      strcat(full_path, menus[current_menu].array[menu_index]);
      strcpy(playing, menus[current_menu].array[menu_index]);
      display.clearDisplay();
      display.setCursor(0, 0);
      display.display();
      play(full_path);
      check_song();
      draw_menu();
      delay_noblock(250);
    } else {
      for (int i = 0; i < 12; i++) {
        if (strcmp(Funcs[i].name,
                  menus[current_menu].array[menu_index]) == 0) {
          delay_noblock(250);
          Funcs[i].call();

          scroll_offset = 0;
          draw_menu();
          delay_noblock(250);
          break;
        }
      }
    }
  }
  //update_time();
}

void update(int direction) {
  //resetting previous line
  display.setTextColor(BLACK);
  display.setCursor(0, CHAR_H * (menu_index - direction));
  display.print(">");
  display.setTextColor(WHITE);
  display.println(menus[current_menu].array[menu_index - direction]);
  display.setCursor(0, CHAR_H * menu_index);
  
  display.print(">");
  display.println(menus[current_menu].array[menu_index]);

  display.display();
}

int arrlen() {
  int i = 0;
  for (i; menus[current_menu].array[i][0] != '\0'; i++);
  return i;
}

void draw_menu() {
  display.clearDisplay();
  int array_size = arrlen();
  if (!array_size) {
    print("There is nothing within this menu!", 0, 0, 0);
    display.display();
    wait();
    return;
  }
  for (int line = 0; line < VISIBLE_LINES; line++) {
    int item = scroll_offset + line;
    if (item >= array_size)
      break;
    display.setCursor(0, line * CHAR_H);
    if (item == menu_index)
      display.print(">");
    else
      display.print(" ");
    display.println(menus[current_menu].array[item]);
  }
  display.display();
}
