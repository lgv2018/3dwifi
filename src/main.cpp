#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <ESP8266WebServer.h>
#include <FS.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>

static const char *config_file_name = "/config.json";
static const char *error_msg_spiffs = "Error opening SPIFFS or /config.json.";
static const char *error_msg_invalid_fmt = "Invalid json format in /config.json";

ESP8266WebServer server(80);    // Create a webserver object that listens for HTTP request on port 80
File fsUploadFile;              // File object to temporary receive a file

static uint32_t gcode_cur_lineno = 1; // last gcode line sent

String getContentType(String filename); // convert the file extension to the MIME type
bool handleFileRead(String path);       // send the right file to the client (if it exists)
void handleFileUpload();                // upload a new file to the SPIFFS]
void handleGcode();
void handleOutOfBandMessage();
void handleSdUpload();

void serial_message(const char *msg, const char *value = NULL) {
  Serial.print("M117 ");
  Serial.print(msg);
  if (value)
    Serial.print(value);
  Serial.println("\n");
}

void fallback_serial_error(const char *error_msg) {
  // send error message before reading SPIFFS config file
  Serial.begin(9600);
  delay(10);
  serial_message(error_msg);
}

void setup() {
  if (!SPIFFS.begin() ||
      !SPIFFS.exists(config_file_name)) {
    fallback_serial_error(error_msg_spiffs);
    return;
  }

  File configFile = SPIFFS.open(config_file_name, "r");
  if (!configFile) {
    fallback_serial_error(error_msg_spiffs);
    return;
  }

  size_t size = configFile.size();
  char *configBuf = new char[size];
  configFile.readBytes(configBuf, size);

  DynamicJsonBuffer configDynBuffer;
  JsonObject& configJson = configDynBuffer.parseObject(configBuf);
  if (!configJson.success()) {
    fallback_serial_error(error_msg_invalid_fmt);
    return;
  }

  Serial.begin(configJson["serial_speed"]);

  const char *dnshn = configJson["mDNS_hostname"];
  if (!MDNS.begin(dnshn))
    serial_message("Error setting mDNS hostname: ", dnshn);

  // setup WIFI
  const char *ssid = configJson["wifi_ssid"];
  const char *passwd = configJson["wifi_password"];
  serial_message("Wifi connecting to ", ssid);
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

  char msgbuff[256];
  snprintf(msgbuff, 256, "SSID: %s, IP %s", ssid, ip.c_str());
  serial_message(msgbuff);

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
  server.on("/msg", HTTP_GET, handleOutOfBandMessage);

  server.on("/sdupload", HTTP_POST, [](){
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.sendHeader("Content-type", "text/plain");
    server.send(200); },
    handleSdUpload);

  server.onNotFound([]() {                              // If the client requests any URI
    if (!handleFileRead(server.uri()))                  // send it if it exists
      server.send(404, "text/plain", "404: Not Found"); // otherwise, respond with a 404 (Not Found) error
  });

  server.begin();                           // Actually start the server
  //serial_message("HTTP server started");
}

void loop() {
  server.handleClient();
}

String getContentType(String filename) { // convert the file extension to the MIME type
  if (filename.endsWith(".html")) return "text/html";
  else if (filename.endsWith(".css")) return "text/css";
  else if (filename.endsWith(".js")) return "application/javascript";
  else if (filename.endsWith(".ico")) return "image/x-icon";
  else if (filename.endsWith(".svg")) return "image/svg+xml";
  else if (filename.endsWith(".gz")) return "application/x-gzip";
  return "text/plain";
}

bool handleFileRead(String path) {
  // send the right file to the client (if it exists)
  String pathWithGz = path + ".gz";

  if (path.endsWith("/"))
    path += "index.html";

   // If the file exists, either as a compressed archive, or normal
   if (SPIFFS.exists(pathWithGz))
     path += ".gz";

  if (SPIFFS.exists(path)) {
    File file = SPIFFS.open(path, "r");
    String contentType = getContentType(path);
    server.streamFile(file, contentType);
    file.close();
    return true;
  }

  return false;
}

void handleOutOfBandMessage() {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.sendHeader("Content-type", "text/plain");
  server.send(200);
  bool last_is_eol = true;
  do {
    size_t len = Serial.available();
    if (len == 0 && !last_is_eol) {
      delay(5);
      continue;
    }
    else if (len > 0) {
      char buf[len+1];
      Serial.readBytes(buf, len);
      last_is_eol = buf[len-1] == '\n';
      buf[len] = 0;
      server.sendContent(buf);
    }
    else
      break;
  } while (true);
}

String send_gcode(const char *cmd, bool waitresp = true, bool checksum = true) {
  String resp;
  char lineno[20];
  char chksumstr[20];

  if (checksum) {
    sprintf(lineno, "N%d ", gcode_cur_lineno);

    // compute checksum
    int checksum = 0;
    int i = 0;
    while(lineno[i]) {
      checksum ^= lineno[i];
      i++;
    }
    i = 0;
    while(cmd[i]) {
      checksum ^= cmd[i];
      i++;
    }
    sprintf(chksumstr, "*%d\n", checksum);
  }

  // check pending messages
  size_t plen = Serial.available();
  if (plen > 0) {
    char buff[plen+1];
    Serial.readBytes(buff, plen);
    resp.concat(buff);
  }

  // send command
  if (checksum)
    Serial.write(lineno);
  Serial.write(cmd);
  if (checksum)
    Serial.write(chksumstr);
  else
    Serial.write("\n");
  Serial.flush();

  //debug
  /*server.sendContent(lineno);
  server.sendContent(cmd);
  server.sendContent(chksumstr);*/

  #define TIMEOUT_READ 200 // ~ 1s
  int timeout = TIMEOUT_READ;
  char last = 0;
  int exitst = 0;
  do {
    size_t len = Serial.available();
    if (len > 0) {
      char buf[len+1];
      buf[len] = 0;

      for(size_t i = 0; i < len; i++) {
        buf[i] = Serial.read();

        // identify message end
        if (buf[i] == ':') exitst = 5; // : identify a message from the firmware, wait for a '\n'
        else if (exitst == 0) {
          if ((last == 'o' && buf[i] == 'k')
            ||(last == '!' && buf[i] == '!')
            ||(last == 'r' && buf[i] == 's')) exitst = 1;
        }
        else if (exitst == 5) {
          if (buf[i] == '\n') exitst = 0;
        }
        else if (exitst == 1) {
          if (buf[i] == '\n') {
            buf[i+1] = 0;
            exitst = 2;
            break;
          }
        }
        last = buf[i];
      }
      resp.concat(buf);
      if (exitst == 2) break;
      timeout = TIMEOUT_READ;
    } else {
      if (!waitresp)
        break;
      delay(5);
      if (--timeout == 0) // timeout
        break;
    }
  } while (true);

  if (!(resp.indexOf("Error:") > 0 || resp.indexOf("Resend:") > 0))
    gcode_cur_lineno++;

  return resp;
}

void handleGcode() {
  String cmd = server.arg("cmd");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.sendHeader("Content-type", "text/plain");
  server.send(200);
  String resp = send_gcode(cmd.c_str());
  server.sendContent(resp);
}

void handleFileUpload() {
  // upload a new file to the SPIFFS

  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    String filename = upload.filename;
    if (!filename.startsWith("/"))
      filename = "/" + filename;

    fsUploadFile = SPIFFS.open(filename, "w");
    filename = String();

  } else if (upload.status == UPLOAD_FILE_WRITE) {
     // Write the received bytes to the file
     if (fsUploadFile)
      fsUploadFile.write(upload.buf, upload.currentSize);

  } else if (upload.status == UPLOAD_FILE_END) {
    if (fsUploadFile) {
      fsUploadFile.close();
      serial_message("Upload ended. Size: ", String(upload.totalSize).c_str());
      server.sendHeader("Location","/index.html");      // Redirect the client to the success page
      server.send(303);
    } else {
      server.send(500, "text/plain", "500: couldn't create file");
    }
  }
}

#define MAX_RAW_SIZE 512 // need to be <= than printer raw size
#define EOT 4
#define ACK 6
#define NCK 21
#define ETB 23
#define CAN 24

char sendBuffer(const char *buffer_line, size_t len, int *retrans) {
  int i = 10;
  do {
    Serial.write((const uint8_t*)buffer_line, len);
    // wait for resp
    int j = 1000;
    while (Serial.available() < 1) {
      delay(5);
      j--;
      if (j == 0) return NCK;
    }

    char resp = Serial.read();
    if (resp != NCK)
      return resp;

    (*retrans)++;
  } while (i--);
  return NCK;
}

void handleSdUpload() {
  // upload a new file to Printer SD CARD

  static char buffer_line[MAX_RAW_SIZE+2];
  static size_t buffer_pos;
  static char resp;
  static char chksum;
  static long start;
  static int error_count;

  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    String filename = upload.filename;
    if (!filename.startsWith("/"))
      filename = "/" + filename;

    chksum = 0;
    buffer_pos = 0;
    resp = ACK;
    error_count = 0;
    start = millis();

    String cmd = "M28 !"; // ! indicates raw transfer
    cmd += filename;
    String response = send_gcode(cmd.c_str());
    server.sendContent(response);
    if (response.indexOf("Error:") >= 0 ||
        response.indexOf("failed") >= 0) {
      server.sendContent(response);
      resp = CAN;
    }

  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (resp != ACK) return;

    for(size_t i = 0; i < upload.currentSize; i++) {
      char c = upload.buf[i];
      buffer_line[buffer_pos++] = c;
      chksum ^= c;
      if (buffer_pos == MAX_RAW_SIZE) {
        buffer_line[buffer_pos++] = ETB; // end of block
        buffer_line[buffer_pos++] = chksum;
        resp = sendBuffer(buffer_line, buffer_pos, &error_count);
        if (resp != ACK) {
          server.sendContent("Error sending block.");
          break;
        }
        buffer_pos = 0;
        chksum = 0;
      }
    }

  } else if (upload.status == UPLOAD_FILE_END) {
    if (resp == ACK && buffer_pos > 0) {
      buffer_line[buffer_pos++] = ETB;
      buffer_line[buffer_pos++] = chksum;
      resp = sendBuffer(buffer_line, buffer_pos, &error_count);
      if (resp != ACK)
        server.sendContent("Error sending last block.");
    }

    bool success = resp == ACK;

    if (resp != EOT && resp != CAN) { // the printer is still running the protocol, finish it
      buffer_pos = 0;
      buffer_line[buffer_pos++] = EOT; // end of transmission
      resp = sendBuffer(buffer_line, buffer_pos, &error_count);
      if (resp != EOT) { // end transmission not ok
        server.sendContent("Error finishing transmission. Reset printer!");
      }
    }

    // send transmission feedback to printer and browser
    long lsecs = (millis() - start) / 1000;
    int mins = lsecs / 60;
    int secs = lsecs % 60;
    sprintf(buffer_line, "Errors: %d\nTime: %d:%d\nStatus: %s!\n",
      error_count, mins, secs, success ? "Success" : "Failed");
    if (success) {
      serial_message("SD upload successed: ", String(upload.totalSize).c_str());
      server.sendContent(String(buffer_line));
    }
    else {
      serial_message("SD upload failed.");
      server.sendContent(String(buffer_line));
    }
  }
}
