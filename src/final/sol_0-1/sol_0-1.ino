#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SD.h>
#include <FS.h>
#include "AudioTools.h"

/*
Sleep intervals
*/
#define SLEEP_INT 60000UL    //ms until screen blanks
#define DSLEEP_INT 120000UL  //ms until deepsleep if nothing is playing
unsigned long start_sleep = 0;
unsigned long start_dsleep = 0;
unsigned long start = 0;

/*
Pin assignments to PCM5102A
*/
#define I2S_BCK_IO  1
#define I2S_LRCK_IO 3
#define I2S_DATA_IO 2

/*
Analog read values for input. Sensitivity is +- current value
*/
#define ANALOG_RESOLUTION 12
#define UP 3600
#define DOWN 2700
#define SELECT 3250
#define BACK 4095
#define WAKE GPIO_NUM_0
#define BUTTONS 0 //analog 0
#define SENSITIVITY 100

/*
Pin assignments for SDCard connection and setup
Semaphore is used for SD usage during song playing
*/
SemaphoreHandle_t sd_mutex;
#define SCK 4
#define MISO 5
#define MOSI 6
#define CS 7
#define SD_SPI_HZ 20000000

/*
Font size and screen dimensions for a Wisevision SSD1306 display, 128x64
*/
#define CHAR_SIZE 1
#define CHAR_W (6 * CHAR_SIZE)
#define CHAR_H (8 * CHAR_SIZE)
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

/*
Maximum amount of songs and their character length to be loaded at once
*/

#define SONG_LIST_HEIGHT = 1024
#define SONG_LIST_LENGTH = 22

/*
Audio buffer declaration for mono and stereo
*/
alignas(4) static uint8_t audio_buffer[4096];
alignas(4) static uint8_t mono_buffer[2048];

/*
Location and song tracking
*/
char working_directory[1024];
char playing[128] = {0};

/*
Hardware setup for audio
*/
I2SStream i2s;
AudioInfo info(44100, 2, 16);

/*
Misc
*/
int current_brightness = 2;
uint8_t brightness_value = 0x7F;


SPIClass spi = SPIClass(FSPI);

struct Execute { //struct to relate names to executables
  const char *name;
  void (*call)(void);
};


/*
Audio Tracking
Using volatile for most up to date song status
*/

struct SongInfo {
  File file;
  char title[128];
  uint32_t sample_rate = 0;
  int byte_arrangement = 0;
  volatile bool paused = false;
  volatile bool active = false;
  volatile bool ended = false;
  volatile bool idle = true; //task is not touching I2S
  volatile uint32_t position = 0;
  volatile uint32_t size = 0;
  uint32_t end_of_file = 0;
  uint32_t start_position = 0;
  uint16_t audio_channels = 2;
};

struct SongInfo Song;

void print(const char *text, int x = 0, int y = 0, int clr = 0) {
  if (clr) display.clearDisplay();
  display.setCursor(x * CHAR_W, y * CHAR_H);
  display.println(text);
}

/*
Audio Engine
*/

void audio_task(void *) {
  while (1) {
    if (!Song.active || Song.paused) {
      Song.idle = true;
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    Song.idle = false;
    size_t bytes_read = 0;
    size_t push_i2s = 0;
    bool stopped = false;
    xSemaphoreTake(sd_mutex, portMAX_DELAY);
    if (!Song.active || !Song.file) {
      stopped = true;
    } else {
      uint32_t pos = Song.position;
      if (pos < Song.end_of_file) {
        uint32_t bytes_remaining = Song.end_of_file - pos;
        if (Song.audio_channels == 1) {
          uint32_t bytes_to_read;
          if (bytes_remaining < sizeof(mono_buffer)) {
            bytes_to_read = bytes_remaining;
          } else {
            bytes_to_read = sizeof(mono_buffer);
          }
          bytes_to_read = bytes_to_read & ~1U; //avoiding reading half a sample by setting the lowest bit to 0
          if (bytes_to_read) {
            bytes_read = Song.file.read(mono_buffer, bytes_to_read);
          } else {
            bytes_read = 0;
          }
          size_t frames = bytes_read / 2;
          int16_t *src = (int16_t *)mono_buffer;
          int16_t *dst = (int16_t *)audio_buffer;
          for (size_t i = 0; i < frames; i++) {
            dst[2 * i] = src[i];
            dst[2 * i + 1] = src[i];
          }
          push_i2s = frames * 4;
        } else {
          uint32_t bytes_to_read;
          if (bytes_remaining < sizeof(mono_buffer)) {
            bytes_to_read = bytes_remaining;
          } else {
            bytes_to_read = sizeof(mono_buffer);
          }
          bytes_to_read = bytes_to_read & ~3U;
          if (bytes_to_read) {
            bytes_read = Song.file.read(mono_buffer, bytes_to_read);
          } else {
            bytes_read = 0;
          }
          push_i2s = bytes_read & ~3U;
        }
      }
    }
    xSemaphoreGive(sd_mutex);
    if (stopped) {
      continue;
    }
    if (push_i2s == 0) {
      Song.ended = true;
      Song.active = false;
      continue;
    }
    i2s.write(audio_buffer, push_i2s);
    Song.position += bytes_read;
  }
}

void end_audio() [
  Song.active = false;
  Song.paused = false;
  unsigned long t = millis();
  while (!Song.idle && millis() - t < 500) vTaskDelay(pdMS_TO_TICKS(1));
  xSemaphoreTake(sd_mutex, portMAX_DELAY);
  if (Song.file) {
    Song.file.close();
  }
  xSemaphoreGive(sd_mutex);
}

/*
Audio Operations
*/

bool open_audio(const char *path) {
  Song.file = SD.open(path);
  if (!Song.file) {
    return false;
  }
  uint8_t header[12];
  uint8_t format[24];
  size_t bytes_read = Song.file.read(header, 12);
  
  if (bytes_read < 12) {
    return false; //invalid header, total file size < 12 bytes
  }
  if (memcmp(header, "RIFF", 4) != 0) {
    return false; //invalid ChunkID
  }
  if (memcmp(&header[8], "WAVE", 4) != 0) {
    return false; //invalid WaveID
  }
  bool found = false;
  uint8_t chunk_header[8];

  while (Song.file.position() + 8 <= Song.file.size()) {
      bytes_read = Song.file.read(chunk_header, 8);

      if (bytes_read != 8) {
          return false; //too small again
      }

      uint32_t chunk_size =
            (uint32_t)chunk_header[4]
          | ((uint32_t)chunk_header[5] << 8)
          | ((uint32_t)chunk_header[6] << 16)
          | ((uint32_t)chunk_header[7] << 24);

      if (chunk_size > Song.file.size() - Song.file.position()) {
          return false;
      }

      if (memcmp(chunk_header, "fmt ", 4) == 0) {

          if (chunk_size < 16) {
              return false;
          }
          bytes_read = Song.file.read(format, 16);
          if (bytes_read != 16) {
              return false;
          }
          found = true;
          break;
      }
      uint32_t skip_size = chunk_size;
      if (skip_size % 2 != 0) {
          skip_size++;
      }
      if (skip_size > Song.file.size() - Song.file.position()) {
          return false;
      }
      Song.file.seek(Song.file.position() + skip_size);
  }

  if (!found) {
      return false;
  Song.audio_channels = 
      (uint32_t)format[2]
    | ((uint32_t)format[3] << 8);
  Song.sample_rate =
      (uint32_t)format[4]
    | ((uint32_t)format[5] << 8)
    | ((uint32_t)format[6] << 16)
    | ((uint32_t)format[7] << 24);
  Song.byte_arrangement = 
      (uint32_t)format[14]
    | ((uint32_t)format[15] << 8);

  uint8_t data[8];
  while (Song.file.position() + 8 <= Song.file.size()) {
    bytes_read = Song.file.read(data, 8);
    if (bytes_read != 8) {
      return;
    }
    
    chunk_size = 
    (uint32_t)data[4]
    | ((uint32_t)data[5] << 8)
    | ((uint32_t)data[6] << 16)
    | ((uint32_t)data[7] << 24);
    if (chunk_size > Song.file.size() - Song.file.position()) {
      return false;
    }
    if (memcmp(data, "data", 4) == 0) {
      Song.start_position = Song.file.position();
      break;
    }
    skip_size = chunk_size;
    if (skip_size % 2 != 0) {
      skip_size++;
    }
    if (skip_size > Song.file.size() - Song.file.position()) {
          return false;
    }
    Song.file.seek(Song.file.position() + skip_size);
  }

  return true;
}

bool play(const char *path) {
  stop_song();
  xSemaphoreTake(sd_mutex, portMAX_DELAY);
  if (!open_audio(path)) {
    //handle error
  }
  xSemaphoreGive(sd_mutex);
  Song.paused = false;
  Song.ended = false;
  Song.active = true;
  return true;
}

/*
Input operations
*/

void wait() {
  delay(100);
  while (get_input() == 0) {
    delay(10);
  }
  delay(100);
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

void setup() {
  Serial.begin(115200);
  analogReadResolution(ANALOG_RESOLUTION);

  //PCM5102A i2s setup
  auto config = i2s.defaultConfig(TX_MODE);
  config.copyFrom(info);
  config.pin_bck = I2S_BCK_IO;
  config.pin_ws = I2S_LRCK_IO;
  config.pin_data = I2S_DATA_IO;
  config.buffer_count = 8;
  config.buffer_size = 1024;
  Serial.println("I2S");
  i2s.begin(config);
  Song.sample_rate = info.sample_rate;

  //ssd1306/1315
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setTextWrap(false);
  
  Wire.setClock(400000);

  display.ssd1306_command(SSD1306_SETCONTRAST);
  display.ssd1306_command(brightness_value);

  delay(100);

  spi.begin(SCK, MISO, MOSI, CS);

  if (!SD.begin(CS, spi, SD_SPI_HZ)) {
    //strlcpy(menu[5], "Remount Card");
    if (SD.cardType() == CARD_NONE) {
      print("No SD card attached", 0, 0, 1);
      display.display();
      delay(2000);
    } else {
      print("SD card mount fail", 0, 0, 1);
      //handle error
    }
  } else {
    display.clearDisplay();
    // get current directories for navigation
  }

  sd_mutex = xSemaphoreCreateMutex();
  xTaskCreate(audio_task, "audio", 6144, nullptr, 5, nullptr);
  
  start_sleep = millis();
  start_dsleep = millis();
  start = millis();
  //display current directory

}

void loop() {
  if (millis() - start >= 100) {
    start = millis();
    exec(get_input());
  }
  if (millis() - start_sleep >= SLEEP_INT) {
    sleepy(1);
  }
}

