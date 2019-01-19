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

String getContentType(String filename); // convert the file extension to the MIME type
bool handleFileRead(String path);       // send the right file to the client (if it exists)
void handleFileUpload();                // upload a new file to the SPIFFS]
void handleGcode();

void serial_message(const char *msg, const char *value = NULL) {
  Serial.print("M117 ");
  Serial.print(msg);
  if (value)
    Serial.print(value);
  Serial.println(";");
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

void handleGcode() {
  String cmd = server.arg("cmd");
  Serial.write(cmd.c_str());
  Serial.write('\n');
  Serial.flush();

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.sendHeader("Content-type", "text/plain");
  server.send(200);

  int timeout = 300;
  do {
    size_t len = Serial.available();
    if (len > 0) {
      char buf[len+1];
      Serial.readBytes(buf, len);
      buf[len] = 0;
      server.sendContent(buf);
    } else {
      delay(10);
      if (--timeout == 0) // timeout
        break;
    }
  } while (true);
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
