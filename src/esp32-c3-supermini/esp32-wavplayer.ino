#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SD.h>
#include <FS.h>
#include "AudioTools.h"

//sleepy 
#define SLEEP_INT 60000UL    //ms until screen blanks
#define DSLEEP_INT 120000UL  //ms until deepsleep if nothing is playing
unsigned long start_sleep = 0;
unsigned long start_dsleep = 0;
unsigned long start = 0;

//pcm5102a
#define I2S_BCK_IO  1
#define I2S_LRCK_IO 3
#define I2S_DATA_IO 2

#define UP 3600
#define DOWN 2700
#define SELECT 3250
#define BACK 4095
#define WAKE GPIO_NUM_0
#define BUTTONS 0
#define SENSITIVITY 100

//sd card
#define SCK 4
#define MISO 5
#define MOSI 6
#define CS 7
#define SD_SPI_HZ 20000000

//ssd1306
#define CHAR_SIZE 1
#define CHAR_W (6 * CHAR_SIZE)
#define CHAR_H (8 * CHAR_SIZE)
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define VISIBLE_LINES (SCREEN_HEIGHT / CHAR_H)

#define ARR_SIZE 10
#define MAX_CHAR 32
#define MAX_FILES 256  //files listed per folder
#define MAX_DIR 64     //folders listed

int scroll_offset = 0;

char song_folders[MAX_DIR][MAX_CHAR] = {0};  //menu 2
char songs[MAX_FILES][MAX_CHAR] = {0};       //menu 3
char current_dir[MAX_CHAR + 2] = {0};
char playing[MAX_CHAR] = {0};
int menu_index = 0;
int current_menu = 0;
int previous_menu = 0;

I2SStream i2s;
AudioInfo info(44100, 2, 16);
uint32_t current_rate = 0;

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
  int max;
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

alignas(4) static uint8_t audio_buf[4096];
alignas(4) static uint8_t mono_buf[2048];

File current_song;
SemaphoreHandle_t sd_mutex;

//set to volatile to force memory reads for most recent update

volatile bool song_active = false; //a song is loaded
volatile bool pause_song = false;
volatile bool track_ended = false;
volatile bool audio_idle = true; //task is not touching I2S
volatile uint32_t audio_pos = 0;
volatile uint32_t audio_total = 0;
uint32_t data_end = 0;
uint16_t file_channels = 2;

//returns true if WAV header is valid 16-bit PCM and file is positioned at audio data.
bool open_wav(const char *path) {
  if (current_song) current_song.close();
  current_song = SD.open(path);
  if (!current_song) return false;

  uint8_t hdr[12];
  if (current_song.read(hdr, 12) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) {
    current_song.close();
    return false;
  }

  uint16_t ch = 2, bits = 16;
  uint32_t rate = 44100;
  bool got_data = false;

  while (current_song.available()) {
    char id[4];
    uint32_t size;
    if (current_song.read((uint8_t *)id, 4) != 4 || current_song.read((uint8_t *)&size, 4) != 4) break;

    if (!memcmp(id, "fmt ", 4)) {
      if (size < 16) break;
      uint8_t f[16];
      if (current_song.read(f, 16) != 16) break;
      ch = f[2] | (f[3] << 8);
      rate = f[4] | (f[5] << 8) | (f[6] << 16) | ((uint32_t)f[7] << 24);
      bits = f[14] | (f[15] << 8);
      current_song.seek(current_song.position() + (size - 16) + (size & 1));
    } else if (!memcmp(id, "data", 4)) {
      audio_total = size;
      data_end = current_song.position() + size;
      uint32_t filesize = current_song.size();
      if (data_end > filesize) data_end = filesize;  //some encoders write bogus sizes
      got_data = true;
      break;
    } else {
      current_song.seek(current_song.position() + size + (size & 1));
    }
  }

  if (!got_data || bits != 16 || ch < 1 || ch > 2) {
    current_song.close();
    return false;
  }

  file_channels = ch;
  if (rate != current_rate) {
    AudioInfo ai(rate, 2, 16);  //always feed the DAC stereo
    i2s.setAudioInfo(ai);
    current_rate = rate;
  }
  audio_pos = 0;
  return true;
}

void audio_task(void *) {
  for (;;) {
    if (!song_active || pause_song) {
      audio_idle = true;
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    audio_idle = false;

    size_t bytes_read = 0;    //bytes read from file
    size_t out = 0;  //bytes to push to I2S
    bool stopped = false;

    xSemaphoreTake(sd_mutex, portMAX_DELAY);
    if (!song_active || !current_song) {
      stopped = true;
    } else {
      uint32_t pos = current_song.position();
      if (pos < data_end) {
        uint32_t left = data_end - pos;
        if (file_channels == 1) {
          uint32_t want = left < sizeof(mono_buf) ? left : sizeof(mono_buf);
          want &= ~1U;
          bytes_read = want ? current_song.read(mono_buf, want) : 0;
          size_t frames = bytes_read / 2;
          int16_t *src = (int16_t *)mono_buf;
          int16_t *dst = (int16_t *)audio_buf;
          for (size_t i = 0; i < frames; i++) {
            dst[2 * i] = src[i];
            dst[2 * i + 1] = src[i];
          }
          out = frames * 4;
        } else {
          uint32_t want = left < sizeof(audio_buf) ? left : sizeof(audio_buf);
          want &= ~3U;
          bytes_read = want ? current_song.read(audio_buf, want) : 0;
          out = bytes_read & ~3U;
        }
      }
    }
    xSemaphoreGive(sd_mutex);

    if (stopped) continue;

    if (out == 0) {  //end of track (or read error)
      track_ended = true;
      song_active = false;
      continue;
    }

    i2s.write(audio_buf, out);  //blocks when DMA is full
    audio_pos += bytes_read;
  }
}

void stop_song() {
  song_active = false;
  pause_song = false;
  //wait for audio task to leave i2s.write()
  unsigned long t = millis();
  while (!audio_idle && millis() - t < 500) vTaskDelay(pdMS_TO_TICKS(1));
  xSemaphoreTake(sd_mutex, portMAX_DELAY);
  if (current_song) current_song.close();
  xSemaphoreGive(sd_mutex);
}

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


bool play(const char *path) {
  stop_song();
  xSemaphoreTake(sd_mutex, portMAX_DELAY);
  bool ok = open_wav(path);
  xSemaphoreGive(sd_mutex);
  if (!ok) {
    print("Open failed", 0, 0, 1);
    print("16-bit WAV only", 0, 1, 0);
    display.display();
    wait();
    return false;
  }
  pause_song = false;
  track_ended = false;
  song_active = true;
  return true;
}

int adjust(int pos) {
  return pos * CHAR_H;
}

void setup() {
  Serial.begin(115200);
  analogReadResolution(12);
  start_sleep = millis();
  start_dsleep = millis();
  start = millis();

  sd_mutex = xSemaphoreCreateMutex();

  auto config = i2s.defaultConfig(TX_MODE);
  config.copyFrom(info);
  config.pin_bck = I2S_BCK_IO;
  config.pin_ws = I2S_LRCK_IO;
  config.pin_data = I2S_DATA_IO;
  config.buffer_count = 8;
  config.buffer_size = 1024;
  Serial.println("I2S");
  i2s.begin(config);
  current_rate = info.sample_rate;

  Serial.println("DISPLAY");
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  Wire.setClock(400000);
  display.ssd1306_command(SSD1306_SETCONTRAST);
  display.ssd1306_command(brightness_value);
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setTextWrap(false);
  print("Checking SD card module..", 0, 0, 1);
  display.display();
  delay(500);

  Serial.println("SPI");
  spi.begin(SCK, MISO, MOSI, CS);
  Serial.println("SD");

  bool sd_ok = SD.begin(CS, spi, SD_SPI_HZ);
  if (!sd_ok) {
    print("Mount Failed", 0, 0, 1);
    strlcpy(menu[5], "Remount Card", MAX_CHAR);
    display.display();
    delay(2000);
  } else if (SD.cardType() == CARD_NONE) {
    print("No SD card attached", 0, 0, 1);
    strlcpy(menu[5], "Remount Card", MAX_CHAR);
    display.display();
    delay(2000);
  } else {
    Serial.println("SD OK");
    display.clearDisplay();
    ls("/", 2);
    Serial.println("LS OK");
  }

  draw_menu();
  Serial.println("MENU OK");

  xTaskCreate(audio_task, "audio", 6144, nullptr, 5, nullptr);
}

void loop() {
  if (millis() - start >= 100) {
    start = millis();
    exec(get_input());
  }
  if (millis() - start_sleep >= SLEEP_INT) {
    sleepy(1);
  }
  delay(2);
}

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
  delay(100);
  while (get_input() == 0) {
    delay(10);
  }
  delay(100);
}

void delay_noblock(int ms) {
  delay(ms);  // audio runs in its own task, so a plain delay no longer starves it
}

int draw_progress(int old_width) {
  if (audio_total == 0) return old_width;
  int width = ((uint64_t)audio_pos * SCREEN_WIDTH) / audio_total;
  if (width <= old_width) {
    return old_width;
  }
  display.fillRect(0, SCREEN_HEIGHT - 4, SCREEN_WIDTH, 4, BLACK);
  display.fillRect(0, SCREEN_HEIGHT - 4, width, 4, WHITE);
  display.display();
  return width;
}

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

void check_song() {
  if (!song_active) {
    print("Nothing playing", 0, 0, 1);
    display.display();
    wait();
    start_sleep = millis();
    return;
  }

  delay(250);
  print(playing, 0, 0, 1);
  display.display();
  if (pause_song) {
    draw_pause();
  }

  int choice = 0;
  int current_width = 0;
  unsigned long last = 0;

  while (choice != 2 && song_active) {
    if (millis() - last >= 150) {
      last = millis();
      choice = get_input();
      if (choice) {
        start_sleep = millis();
        if (choice == 3) {
          pause_song = !pause_song;
          draw_pause();
          delay(250);
        }
      }
    } else if (millis() - start_sleep >= SLEEP_INT) {
      sleepy(0);
      print(playing, 0, 0, 1);
      if (pause_song) {
        draw_pause();
      }
      display.display();
      current_width = 0;
    }
    current_width = draw_progress(current_width);
    delay(5);
  }
  start_dsleep = millis();
  start_sleep = millis();
}

void poweroff() {
  stop_song();
  display.clearDisplay();
  display.display();
  esp_deep_sleep_enable_gpio_wakeup(BIT(WAKE), ESP_GPIO_WAKEUP_GPIO_HIGH);
  esp_deep_sleep_start();
}

void sleepy(int full_sleep) {
  start_dsleep = millis();
  display.ssd1306_command(SSD1306_DISPLAYOFF);
  while (true) {
    if (get_input()) {
      break;
    }
    if (full_sleep && !song_active && millis() - start_dsleep >= DSLEEP_INT) {
      poweroff();
    }
    delay(50);
  }
  display.ssd1306_command(SSD1306_DISPLAYON);
  delay(250);
  start_sleep = millis();
  if (full_sleep) {
    draw_menu();
  }
}

void umount() {
  stop_song();
  memset(songs, 0, sizeof(songs));
  memset(song_folders, 0, sizeof(song_folders));
  strlcpy(menu[5], "Remount Card", MAX_CHAR);
  xSemaphoreTake(sd_mutex, portMAX_DELAY);
  SD.end();
  xSemaphoreGive(sd_mutex);
  print("Card unmounted", 0, 0, 1);
  display.display();
  wait();
}

void remount() {
  Serial.println("[SD] Attempting remount...");

  stop_song();
  xSemaphoreTake(sd_mutex, portMAX_DELAY);
  SD.end();
  delay(100);

  pinMode(CS, OUTPUT);
  digitalWrite(CS, HIGH);

  bool ok = SD.begin(CS, spi, SD_SPI_HZ);
  xSemaphoreGive(sd_mutex);

  if (!ok) {
    Serial.println("[SD] Mount failed");
    print("Mount Failed", 0, 0, 1);
    display.display();
    wait();
    return;
  }

  Serial.println("[SD] Mount successful");
  menu[5][0] = '\0';
  menu_index = 0;
  memset(song_folders, 0, sizeof(song_folders));
  ls("/", 2);
}

static bool is_wav(const char *n) {
  size_t l = strlen(n);
  return l > 4 && n[0] != '.' && strcasecmp(n + l - 4, ".wav") == 0;
}

int add_folder(const char *name) {
  if (name[0] == '.' || strlen(name) >= MAX_CHAR) return 0;
  if (!strcmp(name, "System Volume Information")) return 0;
  int i = 0;
  for (; i < MAX_DIR && song_folders[i][0] != '\0'; i++) {
    if (!strcmp(name, song_folders[i])) {
      return 0;
    }
  }
  if (i >= MAX_DIR) {
    print("Max folders reached", 0, 0, 1);
    print("Allowed: 64", 0, 1, 0);
    display.display();
    wait();
    return 0;
  }
  strlcpy(song_folders[i], name, MAX_CHAR);
  return 1;
}

int add_file(const char *name) {
  if (!is_wav(name) || strlen(name) >= MAX_CHAR) return 0;
  int i = 0;
  for (; i < MAX_FILES && songs[i][0] != '\0'; i++) {
    if (!strcmp(name, songs[i])) {
      return 0;
    }
  }
  if (i >= MAX_FILES) {
    print("Max files reached", 0, 0, 1);
    print("Allowed: 256", 0, 1, 0);
    display.display();
    wait();
    return 0;
  }
  strlcpy(songs[i], name, MAX_CHAR);
  return 1;
}

void ls(const char *name, int menu_type) {
  xSemaphoreTake(sd_mutex, portMAX_DELAY);

  File root = SD.open(name);
  if (!root) {
    xSemaphoreGive(sd_mutex);
    print("Failed to open card", 0, 0, 1);
    display.display();
    delay(1000);
    return;
  }
  if (!root.isDirectory()) {
    root.close();
    xSemaphoreGive(sd_mutex);
    print("Won't open non-dir", 0, 0, 1);
    display.display();
    delay(1000);
    return;
  }

  File file = root.openNextFile();
  while (file) {
    //copy name out so we can release the lock protected file before add calls wait()
    char nm[MAX_CHAR + 8];
    strlcpy(nm, file.name(), sizeof(nm));
    bool is_dir = file.isDirectory();
    file.close();

    if (is_dir) {
      if (menu_type == 2) add_folder(nm);
    } else {
      if (menu_type == 3) add_file(nm);
    }
    file = root.openNextFile();
  }
  root.close();
  xSemaphoreGive(sd_mutex);
}

void make_folder(const char *path, const char *label) {
  display.clearDisplay();
  if (SD.mkdir(path)) {
    print("Created:", 0, 0, 0);
  } else {
    print("Failed to make:", 0, 0, 0);
  }
  print(label, 0, 1, 0);
  display.display();
  delay(500);
}

void format() {
  print("Defaulting folders", 0, 0, 1);
  print("please make sure you", 0, 1, 0);
  print("have formatted this", 0, 2, 0);
  print("card in FAT32", 0, 3, 0);
  print("Otherwise, poweroff", 0, 4, 0);
  print("Press to cont.", 0, 6, 0);
  display.display();
  delay(200);
  wait();

  static const char *names[] = {
    "Rock", "Country", "HipHop", "Lo-Fi", "RnB", "Pop", "Classical", "Misc", "Audio Books"
  };
  char path[40];

  xSemaphoreTake(sd_mutex, portMAX_DELAY);
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    snprintf(path, sizeof(path), "/%s", names[i]);
    make_folder(path, names[i]);
  }
  xSemaphoreGive(sd_mutex);

  memset(song_folders, 0, sizeof(song_folders));
  ls("/", 2);
}

void settings() {
  previous_menu = current_menu;
  current_menu = 1;
  menu_index = 0;
  scroll_offset = 0;
}

void select_songs() {
  previous_menu = current_menu;
  current_menu = 2;
  menu_index = 0;
  scroll_offset = 0;
  draw_menu();
}

static void draw_brightness_screen() {
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

void set_brightness() {
  delay(100);
  draw_brightness_screen();

  unsigned long last = millis();
  while (1) {
    if (millis() - last >= 200) {
      last = millis();
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
        draw_brightness_screen();
      }
    }
    delay(5);
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

void set_sleep() {}

void battery_info() {}

void diag() {
  print("Memory Stats:", 0, 0, 1);
  display.setCursor(0, adjust(2));
  display.printf("Total:%dkb", (int)(ESP.getHeapSize() / 1024));
  display.setCursor(0, adjust(3));
  display.printf("Free:%dkb", (int)(ESP.getFreeHeap() / 1024));
  display.setCursor(0, adjust(4));
  display.printf("Aval:%dkb", (int)(ESP.getMaxAllocHeap() / 1024));
  display.setCursor(0, adjust(5));
  display.printf("Audio stack:%d", (int)uxTaskGetStackHighWaterMark(nullptr));
  display.display();
  delay(1000);
  wait();
}

struct Menus menus[] = {
  {0, menu, ARR_SIZE},
  {1, settings_menu, ARR_SIZE},
  {2, song_folders, MAX_DIR},
  {3, songs, MAX_FILES}
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

int arrlen() {
  int i = 0;
  int mx = menus[current_menu].max;
  while (i < mx && menus[current_menu].array[i][0] != '\0') i++;
  return i;
}

void draw_menu() {
  display.clearDisplay();
  int array_size = arrlen();
  if (!array_size) {
    print("There is nothing", 0, 0, 0);
    print("within this menu!", 0, 1, 0);
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

void play_random() {
  int idx[MAX_DIR];
  int n = 0;
  for (int i = 0; i < MAX_DIR; i++) {
    if (song_folders[i][0] != '\0') idx[n++] = i;
  }
  if (!n) {
    print("No folders found", 0, 0, 1);
    display.display();
    wait();
    return;
  }
  int count = 0;
  for (int tries = 0; tries < n && !count; tries++) {
    int pick = idx[random(0, n)];
    snprintf(current_dir, sizeof(current_dir), "/%s", song_folders[pick]);
    memset(songs, 0, sizeof(songs));
    ls(current_dir, 3);
    int c = 0;
    while (c < MAX_FILES && songs[c][0] != '\0') c++;
    count = c;
  }
  if (!count) {
    print("No songs found", 0, 0, 1);
    display.display();
    wait();
    return;
  }

  int pick = random(0, count);
  char full_path[MAX_CHAR * 2 + 4];
  snprintf(full_path, sizeof(full_path), "%s/%s", current_dir, songs[pick]);
  strlcpy(playing, songs[pick], MAX_CHAR);

  if (play(full_path)) {
    check_song();
  }
  current_menu = 0;
  menu_index = 0;
  scroll_offset = 0;
  draw_menu();
  delay(250);
}

void exec(int choice) {
  if (!choice) {
    return;
  }
  start_sleep = millis();

  if (choice == 2) {
    back();
    draw_menu();
    return;
  }

  int array_size = arrlen();

  if (choice == 1 || choice == -1) {
    menu_index += choice;

    if (menu_index < 0) {
      menu_index = 0;
    } else if (menu_index >= array_size) {
      menu_index = array_size - 1;
    }
    if (menu_index < 0) menu_index = 0;

    if (menu_index < scroll_offset) {
      scroll_offset = menu_index;
    }
    if (menu_index >= scroll_offset + VISIBLE_LINES) {
      scroll_offset = menu_index - VISIBLE_LINES + 1;
    }

    draw_menu();
  } else if (choice == 3) {
    if (!array_size) return;

    if (current_menu == 2) {
      snprintf(current_dir, sizeof(current_dir), "/%s", song_folders[menu_index]);
      memset(songs, 0, sizeof(songs));
      ls(current_dir, 3);
      previous_menu = current_menu;
      current_menu = 3;
      menu_index = 0;
      scroll_offset = 0;
      draw_menu();
      delay(250);
    } else if (current_menu == 3) {
      char full_path[MAX_CHAR * 2 + 4];
      snprintf(full_path, sizeof(full_path), "%s/%s", current_dir, songs[menu_index]);
      strlcpy(playing, songs[menu_index], MAX_CHAR);
      display.clearDisplay();
      display.setCursor(0, 0);
      display.display();
      if (play(full_path)) {
        check_song();
      }
      draw_menu();
      delay(250);
    } else {
      for (size_t i = 0; i < sizeof(Funcs) / sizeof(Funcs[0]); i++) {
        if (strcmp(Funcs[i].name, menus[current_menu].array[menu_index]) == 0) {
          delay(250);
          Funcs[i].call();

          scroll_offset = 0;
          draw_menu();
          delay(250);
          break;
        }
      }
    }
  }
}
