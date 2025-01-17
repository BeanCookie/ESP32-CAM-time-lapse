#include "esp_http_client.h"
#include "esp_camera.h"
#include "driver/rtc_io.h"
#include <HTTPClient.h>
#include "Arduino.h"
#include <BlynkSimpleEsp32.h>
#include <TimeLib.h>
#include <WidgetRTC.h>
#include <EEPROM.h>
#include "driver/adc.h"
#include "../src/settings.cpp"

// Disable brownout problems
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// https://randomnerdtutorials.com/esp32-cam-video-streaming-face-recognition-arduino-ide/
// https://loboris.eu/ESP32/ESP32-CAM%20Product%20Specification.pdf

// https://robotzero.one/time-lapse-esp32-cameras/
// https://robotzero.one/wp-content/uploads/2019/04/Esp32CamTimelapsePost.ino

// is alarm (based on other ESP32 device - Beehive alarm controller)
bool isAlarm = true;

Settings settings;

WiFiClient client;

// Deep sleep interval in seconds
int deep_sleep_interval = 10;

// Deep sleep alarm interval in seconds
int deep_sleep_alarm_interval = 10;

// Use flash
bool use_flash = false;

// Take image depends on current time
bool use_rtc = true;

// maxHour 22:00
int max_hour = 22;
int max_minute = 0;

// min hour 6:00
int min_hour = 6;
int min_minute = 0;

// Device is connected to WiFi/Blynk and camera is setuped
bool device_connected_and_prepared = false;

// CAMERA_MODEL_AI_THINKER
// https://github.com/espressif/arduino-esp32/blob/master/libraries/ESP32/examples/Camera/CameraWebServer/camera_pins.h

#define CAMERA_MODEL_AI_THINKER
#define PWDN_GPIO_NUM     32      // power to camera (on/off)
#define RESET_GPIO_NUM    -1      // -1 = not used
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26      // i2c sda
#define SIOC_GPIO_NUM     27      // i2c scl
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25      // vsync_pin
#define HREF_GPIO_NUM     23      // href_pin
#define PCLK_GPIO_NUM     22      // pixel_clock_pin

// 1 byte in EEPROM
#define EEPROM_SIZE 1
#define WIFI_ATTEMPTS_COUNT 5

// You can use all GPIOs of the header on the button side as analog inputs (12-15,2,4).
// You can define any GPIO pair as your I2C pins, just specify them in the Wire.begin() call.
// BUT analog pins doesn't works after using  WiFi.Begin() !!!

// V0 - deep sleep interval in seconds slider
// V1 - Blynk Image gallery, sended image
// V2 - Terminal
// V3 - deep sleep interval result
// V4 - use flash
// V5 - Local IP
// V6 - WIFi signal
// V7 - version
// V8 - current time
// V9 - setted max/min time
// V10 - time input
// V11 - use rtc
// V12 - is alarm
// V13 - status

// Attach Blynk virtual serial terminal
WidgetTerminal terminal(V2);

// Get real time
WidgetRTC rtc;

// Synchronize settings from Blynk server with device when internet is connected
BLYNK_CONNECTED()
{
  Serial.println("Blynk synchronized");
  rtc.begin();
  Blynk.syncAll();
}

// deep sleep interval in seconds
BLYNK_WRITE(V0)
{
  if (param.asInt())
  {
    deep_sleep_interval = param.asInt();
    Serial.println("Deep sleep interval was set to: " + String(deep_sleep_interval));
    Blynk.virtualWrite(V3, String(deep_sleep_interval));
  }
}

// set image capture time interval (min and max hour:minute)
BLYNK_WRITE(V10)
{
  TimeInputParam t(param);

  if (t.hasStartTime())
  {
    min_hour = t.getStartHour();
    min_minute = t.getStartMinute();
  }

  if (t.hasStopTime())
  {
    max_hour = t.getStopHour();
    max_minute = t.getStopMinute();
  }
}

// LED flash
BLYNK_WRITE(V4)
{
  if (param.asInt() == 1)
  {
    use_flash = true;
    Serial.println("LED flash was enabled.");
    Blynk.virtualWrite(V4, false);
  }
}

// RTC - use or not use a real time
BLYNK_WRITE(V11)
{
  use_rtc = param.asInt();
}

// Terminal input
BLYNK_WRITE(V2)
{
  String valueFromTerminal = param.asStr();

  if (String("clear") == valueFromTerminal)
  {
    terminal.clear();
    terminal.println("CLEARED");
    terminal.flush();
  }
  else if (String("restart") == valueFromTerminal || String("reset") == valueFromTerminal)
  {
    terminal.clear();
    terminal.println("Restart, bye");
    terminal.flush();
    ESP.restart();
  }
  else if (valueFromTerminal != "\n" || valueFromTerminal != "\r" || valueFromTerminal != "")
  {
    terminal.println(String("unknown command: ") + valueFromTerminal);
    terminal.flush();
  }
}

int get_deep_sleep_interval()
{
  return deep_sleep_interval;
}

bool init_wifi()
{
  int connAttempts = 0;
  Serial.println("\r\nConnecting to: " + String(settings.wifiSSID));
  // try config - quicker for WiFi connection
  WiFi.begin(settings.wifiSSID, settings.wifiPassword);

  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
    if (connAttempts > 20)
      return false;
    connAttempts++;
  }
  Serial.println("\r\nConnecting to: " + String(settings.wifiSSID) + " OK");
  return true;
}

bool init_blynk()
{
  Blynk.begin(settings.blynkAuth, settings.wifiSSID, settings.wifiPassword, settings.blynkDomain, settings.blynkPort);
  // timeout v milisekundach * 3
  return Blynk.connected();
}

bool init_camera()
{
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;               // XCLK 20MHz or 10MHz for OV2640 double FPS (Experimental)
  config.pixel_format = PIXFORMAT_JPEG;              // Options =  YUV422, GRAYSCALE, RGB565, JPEG, RGB888
  config.frame_size = FRAMESIZE_SXGA;           // Image sizes: 160x120 (QQVGA), 128x160 (QQVGA2), 176x144 (QCIF), 240x176 (HQVGA), 320x240 (QVGA),
                                                //              400x296 (CIF), 640x480 (VGA, default), 800x600 (SVGA), 1024x768 (XGA), 1280x1024 (SXGA),
                                                //              1600x1200 (UXGA)
  config.jpeg_quality = 20;                     // 0-63 lower number means higher quality (can cause failed image capture if set too low at higher resolutions)
  config.fb_count = 1;                          // if more than one, i2s runs in continuous mode. Use only with JPEG

  // check the esp32cam board has a psram chip installed (extra memory used for storing captured images)
  //    Note: if not using "AI thinker esp32 cam" in the Arduino IDE, SPIFFS must be enabled
  if (!psramFound()) {
    Serial.println("Warning: No PSRam found so defaulting to image size 'VGA'");
    config.frame_size = FRAMESIZE_VGA;
  }

  // camera init
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK)
  {
    String problem = "";

    switch (err)
    {
    case ESP_FAIL:
      problem = "Generic esp_err_t code indicating failure";
      break;
    case ESP_ERR_NO_MEM:
      problem = "Out of memory";
      break;
    case ESP_ERR_INVALID_ARG:
      problem = "Invalid argument";
      break;
    case ESP_ERR_INVALID_STATE:
      problem = "Invalid state";
      break;
    case ESP_ERR_INVALID_SIZE:
      problem = "Invalid size";
      break;
    case ESP_ERR_NOT_FOUND:
      problem = "Requested resource not found";
      break;
    case ESP_ERR_NOT_SUPPORTED:
      problem = "Operation or feature not supported";
      break;
    case ESP_ERR_TIMEOUT:
      problem = "Operation timed out";
      break;
    case ESP_ERR_INVALID_RESPONSE:
      problem = "Received response was invalid";
      break;
    case ESP_ERR_INVALID_CRC:
      problem = "CRC or checksum was invalid";
      break;
    case ESP_ERR_INVALID_VERSION:
      problem = "Version was invalid";
      break;
    case ESP_ERR_INVALID_MAC:
      problem = "MAC address was invalid";
      break;
    default:
      problem = "Unknown error";
      break;
    }

    Serial.println(problem);

    if (init_blynk())
    {
      Serial.println("Blynk connected OK, wait to sync");
      Blynk.run();
      // delay for Blynk sync
      delay(2000);

      Blynk.virtualWrite(V13, problem);
    }
    return false;
  }
  else
  {
    return true;
  }
}

esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
  switch (evt->event_id)
  {
  case HTTP_EVENT_ERROR:
    Serial.println("HTTP_EVENT_ERROR");
    break;
  case HTTP_EVENT_ON_CONNECTED:
    Serial.println("HTTP_EVENT_ON_CONNECTED");
    break;
  case HTTP_EVENT_HEADER_SENT:
    Serial.println("HTTP_EVENT_HEADER_SENT");
    break;
  case HTTP_EVENT_ON_HEADER:
    Serial.println();
    Serial.printf("HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
    break;
  case HTTP_EVENT_ON_DATA:
    Serial.println();
    Serial.printf("HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
    break;
  case HTTP_EVENT_ON_FINISH:
    Serial.println("");
    Serial.println("HTTP_EVENT_ON_FINISH");
    break;
  case HTTP_EVENT_DISCONNECTED:
    Serial.println("HTTP_EVENT_DISCONNECTED");
    break;
  }
  return ESP_OK;
}

static esp_err_t take_send_photo()
{
  Serial.println("Taking picture...");
  camera_fb_t *fb = NULL;

  // LED flash blick
  if (use_flash)
  {
    // disable hold (prevents led blinking)
    rtc_gpio_hold_dis(GPIO_NUM_4);
    digitalWrite(GPIO_NUM_4, HIGH);
    delay(500);
  }

  fb = esp_camera_fb_get();
  if (!fb)
  {
    Serial.println("Camera capture failed");
    Blynk.virtualWrite(V13, "Camera capture failed");
    return ESP_FAIL;
  }

  // turn of LED flash
  if (use_flash)
  {
    digitalWrite(GPIO_NUM_4, LOW);
    // enable hold (prevents led blinking)
    rtc_gpio_hold_en(GPIO_NUM_4);
  }

  esp_http_client_handle_t http_client;

  esp_http_client_config_t config_client = {0};
  config_client.url = settings.imageUploadScriptUrl;
  config_client.event_handler = _http_event_handler;
  config_client.method = HTTP_METHOD_POST;
  config_client.timeout_ms = 5000;

  http_client = esp_http_client_init(&config_client);

  esp_http_client_set_post_field(http_client, (const char *)fb->buf, fb->len);
  esp_http_client_set_header(http_client, "Content-Type", "image/jpg");

  Serial.println("Sending picture to the server...");
  esp_err_t err = esp_http_client_perform(http_client);
  if (err == ESP_OK)
  {
    Serial.print("HTTP status code OK: ");
    Serial.println(esp_http_client_get_status_code(http_client));
  }
  else
  {
    String errorMessage = "HTTP status error: " + String(err);
    Serial.println(errorMessage);
    Blynk.virtualWrite(V13, errorMessage);
  }

  esp_http_client_cleanup(http_client);
  esp_camera_fb_return(fb);
  return err;
}

bool checkLowerTime()
{
  return min_hour < hour() || (min_hour == hour() && min_minute <= minute());
}

bool checkHigherTime()
{
  return max_hour > hour() || (max_hour == hour() && max_minute >= minute());
}

void waitTakeSendPhoto()
{
  // delay makes more bright picture (camera has time to boot on)
  Serial.println("Waiting for taking camera picture.");
  delay(1000);
  take_send_photo();
}

void setup()
{
  //disable brownout detector
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  Serial.begin(115200);

  // initialize EEPROM with predefined size
  EEPROM.begin(EEPROM_SIZE);
  if (init_camera())
  {
    Serial.println("Camera OK");

    Serial.println("Internet connected, connect to Blynk");
    if (init_blynk())
    {
      Serial.println("Blynk connected OK, wait to sync");
      Blynk.run();

      delay(2000);

      device_connected_and_prepared = true;
      Serial.println("Setup done");

      EEPROM.write(0, 0);
      EEPROM.commit();
    }
    else
    {
      Serial.println("Blynk failed");
    }
  }
  else
  {
    // camera problem - is logged in method..
    Serial.println("camera problem ");
  }
}

void loop()
{
  if (device_connected_and_prepared)
  {
    Serial.println("Set values to Blynk");
    Blynk.virtualWrite(V13, "OK");

    Blynk.virtualWrite(V5, "IP: " + WiFi.localIP().toString() + "|G: " + WiFi.gatewayIP().toString() + "|S: " + WiFi.subnetMask().toString() + "|DNS: " + WiFi.dnsIP().toString());
    Blynk.virtualWrite(V6, WiFi.RSSI());
    Blynk.virtualWrite(V7, settings.version);

    String currentTime = String(hour()) + ":" + minute();
    String minMaxSettedTime = String(min_hour) + ":" + String(min_minute) + " " + String(max_hour) + ":" + String(max_minute);
    Serial.println(minMaxSettedTime);

    Blynk.virtualWrite(V8, currentTime);
    Blynk.virtualWrite(V9, minMaxSettedTime);

    // use flash - take capture always
    if (use_flash)
    {
      // camera LED flashlight
      pinMode(GPIO_NUM_4, OUTPUT);
      Serial.println("Use flash");
      waitTakeSendPhoto();
    }
    // don't user real time - take capture always
    else if (!use_rtc)
    {
      waitTakeSendPhoto();
    }
    // check if time is OK
    else if (checkLowerTime() && checkHigherTime())
    {
      waitTakeSendPhoto();
    }
    // outside of time interval
    else
    {
      waitTakeSendPhoto();
      Serial.println("Capture image");
    }
  }
  else
  {
    Serial.println("Camera or internet connection is not ready");
  }

  Blynk.disconnect();
  WiFi.disconnect();
  Serial.println("Disconnected WiFi and Blynk done, go to sleep for " + String(get_deep_sleep_interval()) + " seconds.");
  adc_power_release();
  esp_deep_sleep(get_deep_sleep_interval() * 1000000);
}

// 5.9.2019 21:00 4.11V
// 6.9.2019 21:00 3.94V
