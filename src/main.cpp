#define FS_NO_GLOBALS // to coexist with SdFat
#include <FS.h>

#include <ESP8266mDNS.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <ArduinoOTA.h>

#include "web.h"

sdfat::SdFat SD;
bool SD_mounted;

const char *config_file_name = "/config.json";
const char *error_msg_spiffs = "Error opening SPIFFS or /config.json.";
const char *error_msg_microsd = "Error mounting SD Card.";
const char *error_msg_invalid_fmt = "Invalid json format in /config.json";

#define SD_CS_PIN 16
    
void serial_message(const char *msg, const char *value) {
  String aux("M117 ");
  aux.concat(msg);
  if (value) {
    aux.concat(" ");
    aux.concat(value);
  }
  send_gcode_raw(aux.c_str(), false, 0);
}

void fallback_serial_error(const char *error_msg) {
  // send error message before reading SPIFFS config file
  Serial.begin(9600);
  delay(10);
  serial_message(error_msg);
}

void setup() {

  /*Serial.begin(115200);
  for(int pin = 22; pin >= 1; pin--) {
    if (pin == 11) continue;
    if (pin == 8) continue;
    Serial.println(pin);
    pinMode(pin, OUTPUT);
    int i = 5;
    while(i--) {
      digitalWrite(pin, HIGH);
      delay(500);
      digitalWrite(pin, LOW);
      delay(500);
    }
  }*/

  system_update_cpu_freq(160);

  if (!SPIFFS.begin() ||
      !SPIFFS.exists(config_file_name)) {
    fallback_serial_error(error_msg_spiffs);
    return;
  }

  fs::File configFile = SPIFFS.open(config_file_name, "r");
  if (!configFile) {
    fallback_serial_error(error_msg_spiffs);
    return;
  }

  size_t size = configFile.size();
  char *configBuf = new char[size];
  configFile.readBytes(configBuf, size);

  DynamicJsonDocument configJson(1024);
  DeserializationError error = deserializeJson(configJson, configBuf);
  if (error) {
    fallback_serial_error(error_msg_invalid_fmt);
    return;
  }

  Serial.begin(configJson["serial_speed"]);

  /* ESP8266 bootloader sends garbage to Marlin while
   * booting. The code below clears the read buffer
   * on startup and reset line number. We assume
   * Marlin can 'ignore' the garbage. The only viable 
   * solution other than that appears to be replacing 
   * the ESP bootloader.
   */
  /*size_t len = Serial.available();
  while(len--) Serial.read();*/

  delay(2000); // wait printer ready

  // reset command number after start
  send_gcode("\nM110 N0", false);
  gcode_cur_lineno = 1;

  const char *dnshn = configJson["mDNS_hostname"];
  if (!MDNS.begin(dnshn))
    serial_message("Error setting mDNS hostname: ", dnshn);

  // setup WIFI
  const char *ssid = configJson["wifi_ssid"];
  const char *passwd = configJson["wifi_password"];
  serial_message("Wifi connecting to", ssid);
  const bool isap = configJson["wifi_ap"];
  bool need_fallback = false;

  if (isap) {
    need_fallback = !WiFi.softAP(ssid, passwd);
  }
  else {
    WiFi.begin(ssid, passwd);

    int i = 0;
    while (WiFi.status() != WL_CONNECTED) { // Wait for the Wi-Fi to connect
      delay(100);
      i++;
      // fallback to AP mode if waiting for more than 30s
      if (i > 300) {
        need_fallback = true;
        break;
      }
    }
  }

  String ip;
  if (need_fallback) {
    ssid = "3DAP";
    WiFi.softAP(ssid);
    serial_message("Fallback. AP mode.");
    ip = WiFi.softAPIP().toString();
  }
  else if (isap)
    ip = WiFi.softAPIP().toString();
  else
    ip = WiFi.localIP().toString();

  //SD_mounted = SD.begin(SD_CS_PIN, SPI_HALF_SPEED);
  SD_mounted = SD.begin(SD_CS_PIN);
  if (!SD_mounted) {
    serial_message(error_msg_microsd);
    SD.initErrorPrint();
    //delay(2000);
  } else {
    /*float cardSize = SD.card()->cardSize()*512.0e-9;
    float freeSize = SD.vol()->freeClusterCount() * SD.vol()->blocksPerCluster() * 512.0e-9; 
    snprintf(buffer, BUFF_SIZE, "SD %.1fGB free/%.1fGB", freeSize, cardSize);
    serial_message(buffer);
    delay(2000);*/
    serial_message("SD Card ready.");
  }

  snprintf(buffer, BUFF_SIZE, "%s in %s", ip.c_str(), ssid);
  serial_message(buffer);

  // setup Web Server

  server.on("/upload", HTTP_GET, []() {                 // if the client requests the upload page
    if (!handleFileRead("/upload.html"))                // send it if it exists
      server.send(404, "text/plain", "404: Not Found"); // otherwise, respond with a 404 (Not Found) error
  });

  server.on("/upload", HTTP_POST,                       // if the client posts to the upload page
    [](){ server.send(200); },                          // Send status 200 (OK) to tell the client we are ready to receive
    handleFileUpload                                    // Receive and save the file
  );

  server.on("/gcode", HTTP_GET, handleGcode);
  server.on("/gcodestatus", HTTP_GET, handleGcodeStatus);
  server.on("/msg", HTTP_GET, handleGetPrinterMessages);

  server.on("/getsdfiles", HTTP_GET, handleGetSdFiles);
  server.on("/removesdfile", HTTP_GET, handleRemoveSdFile);
  server.on("/printsdfile", HTTP_GET, handlePrintSdFile);
  server.on("/cancelsdprint", HTTP_GET, handleCancelSdPrint);
  server.on("/printstatus", handlePrintSdStatus);
  
  server.on("/sdupload", HTTP_POST, [](){
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.sendHeader("Content-type", "text/plain");
    server.send(200); },
    handleSDFileUpload);

  server.onNotFound([]() {                              // If the client requests any URI
    if (!handleFileRead(server.uri()))                  // send it if it exists
      server.send(404, "text/plain", "404: Not Found"); // otherwise, respond with a 404 (Not Found) error
  });

  server.begin();                           // Actually start the server
  //serial_message("HTTP server started");

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    snprintf(buffer, BUFF_SIZE, "OTA: %u%%", (progress / (total / 100)));
    serial_message(buffer);
  });

  ArduinoOTA.onError([](ota_error_t error) {
    const char *errordesc = NULL;
    if (error == OTA_AUTH_ERROR) errordesc = "Auth Failed";
    else if (error == OTA_BEGIN_ERROR) errordesc = "Begin Failed";
    else if (error == OTA_CONNECT_ERROR) errordesc = "Connect Failed";
    else if (error == OTA_RECEIVE_ERROR) errordesc = "Receive Failed";
    else if (error == OTA_END_ERROR) errordesc = "End Failed";
    snprintf(buffer, BUFF_SIZE, "OTA Error[%u]: %s", error, errordesc);
    serial_message(buffer);
  });
  ArduinoOTA.begin();
}

void loop() {
  // read printer messages
  readSerialMessages();

  // if printing, put commands to the printer
  bool can_continue_sending = false;
  if (cmd_queue_resend != -1)
    resend_gcode();
  else
    can_continue_sending = sendPrinterCommandIfPrinting();

  if (!can_continue_sending) {
    // handle HTTP request
    server.handleClient();
  
    // firmware upgrade over the air
    if (!printingFile.isOpen())
      ArduinoOTA.handle();
  }
}
