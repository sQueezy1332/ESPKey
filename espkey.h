#pragma once
/* ESPKey
*  Kenny McElroy
*
* This sketch runs on the ESPKey ESP8266 module carrier with the
* intent of being physically attached to the Wiegand data wires
* between a card reader and the local control box.
*
* Huge thanks to Brad Antoniewicz for sharing his Wiegand tools
* for Arduino.  This code was a great starting point and
* excellent reference: https://github.com/brad-anton/VertX
*
*/
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ESP8266HTTPUpdateServer.h>
#include <FS.h>
#include <ArduinoOTA.h>
#include <GSON.h>
#include <StringUtils.h>

#define VERSION "131"
#define HOSTNAME "ESPKey-" // Hostname prefix for DHCP/OTA
#define CONFIG_FILE "/config.json"
#define AUTH_FILE "/auth.txt"
#define DBG_OUTPUT_PORT Serial	// This could be a file with some hacking
#define CARD_LEN 4     // minimum card length in bits
#define PULSE_WIDTH 200 // length of asserted pulse in microSeconds
#define PULSE_GAP 2000 - PULSE_WIDTH   // delay between pulses in microSeconds

// Pin number assignments
//#define D0_ASSERT 12
#define D0_SENSE 13
//#define D1_ASSERT 16
#define D1_SENSE 14
//#define LED_ASSERT 4
#define LED_SENSE 5
#define CONF_RESET 0

#if defined ESP32
#define gWrite(pin, x) gpio_set_level(pin,x)
#elif defined ESP8266 
#define	gWrite(pin, x) gpio_set_level(pin, x)
#endif
#define DEBUG_ENABLE
#ifdef DEBUG_ENABLE
#define DEBUG(x) Serial.print(x)
#define DEBUGLN(x) Serial.println(x)
#define DEBUGF(x, ...) Serial.printf(x , ##__VA_ARGS__)
#define CHECK_(x) ESP_ERROR_CHECK_WITHOUT_ABORT(x);
#else
#define DEBUG(x)
#define DEBUGLN(x) 
#define DEBUGF(x, ...)
#define NDEBUG
#define CHECK_(x) (void)(x);
#endif

// Default settings used when no configuration file exists
char log_name[20] = "Alpha";
bool ap_enable = true;
bool ap_hidden = false;
char ap_ssid[20] = "ESPKey-config"; // Default SSID.
IPAddress ap_ip(192, 168, 4, 1);
char ap_psk[20] = "accessgranted"; // Default PSK.
char station_ssid[20] = "";
char station_psk[20] = "";
char mDNShost[20] = "ESPKey";
String DoS_id = "7fffffff:31";
char ota_password[24] = "ExtraSpecialPassKey";
char www_username[20] = "";
char www_password[20] = "";
IPAddress syslog_server(0, 0, 0, 0);
unsigned int syslog_port = 514;
char syslog_service_name[20] = "accesscontrol";
char syslog_host[20] = "ESPKey";
byte syslog_priority = 36;

WiFiUDP Udp;
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;
File fsUploadFile;

//byte incoming_byte = 0; 
unsigned long config_reset_millis = 30000;

volatile byte reader1_byte = 0;
String reader1_string = "";
volatile int reader1_count = 0;
volatile unsigned long reader1_millis = 0;
volatile unsigned long aux_change = 0;
volatile byte last_aux = 1;
volatile byte expect_aux = 2;

void IRAM_ATTR reader1_D0_trigger(void) {
	reader1_millis = millis();
	reader1_count++;
	if (!(reader1_count & 3)) { //%4
		reader1_string += String(reader1_byte, HEX);
		reader1_byte = 0;
		return;
	}
	reader1_byte <<= 1;
}
void IRAM_ATTR reader1_D1_trigger(void) {
	reader1_byte |= 1;
	reader1_D0_trigger();
}

byte char_to_byte(char in) {
	if (in >= '0' && in <= '9') return in ^ 0x30;
	else if (in >= 'A' && in <= 'F') return in - 55;
	else if (in >= 'a' && in <= 'f') return in - 87;
	return in;
}

char c2h(byte c) {
	//DEBUGLN("c2h: " + String(BYTE));
	return "0123456789ABCDEF"[0xF & c];
}

void fix_reader1_string() {
	byte loose_bits = reader1_count & 3; //%4
	if (loose_bits) {
		byte moving_bits = 4 - loose_bits;
		byte moving_mask = (1 << moving_bits) - 1;
		DEBUGLN("lb: " + String(loose_bits) + " mb: " + String(moving_bits) + " mm: " + String(moving_mask, HEX));
		byte BYTE = char_to_byte(reader1_string.charAt(0));
		uint32_t str_len = reader1_string.length();
		for (uint32_t i = 0; i < str_len; i++) {
			reader1_string.setCharAt(i, c2h(BYTE >> moving_bits));
			BYTE &= moving_mask;
			BYTE = (BYTE << 4) | char_to_byte(reader1_string.charAt(i + 1));
			DEBUGLN("BYTE: " + String(BYTE, HEX) + " i: " + String(reader1_string.charAt(i)));
		}
		reader1_string += String((BYTE >> moving_bits) | reader1_byte, HEX);
	}
	reader1_string += ":" + String(reader1_count);
}

void reader1_reset() {
	reader1_byte = 0;
	reader1_count = 0;
	reader1_string = "";
}

byte toggle_pin(byte pin) {
	byte new_value = !digitalRead(pin);
	if (pin == LED_SENSE) expect_aux = !new_value;
	digitalWrite(pin, new_value);
	return new_value;
}

void transmit_assert(const byte pin) {
	digitalWrite(pin, HIGH);
	delayMicroseconds(PULSE_WIDTH);
	digitalWrite(pin, LOW);
	delayMicroseconds(PULSE_GAP);
}

void transmit_id_nope(uint32_t sendValue, byte bitcount) {
	DEBUGLN("[-] Sending Data: " + String(sendValue, HEX) + ':' + String(bitcount)); DEBUG('\t');
	for (short x = bitcount - 1; x >= 0; x--) {
		if (bitRead(sendValue, x)) transmit_assert(D1_SENSE);
		else transmit_assert(D0_SENSE);
	}
	DEBUGLN();
}

void transmit_id(String sendValue, uint32_t bitcount) {
	DEBUGLN("Sending data: " + sendValue + ":" + String(bitcount));
	uint32_t bits_available = sendValue.length() << 2; //* 4
	uint32_t excess_bits = 0;
	if (bits_available > bitcount) {
		excess_bits = bits_available - bitcount;
		sendValue = sendValue.substring(excess_bits >> 2); // / 4
		excess_bits &= 3; //%= 4;
		DEBUG("sending: " + sendValue + " with excess bits: " + excess_bits + "\n\t");
	}
	else if (bits_available < bitcount) {
		for (uint32_t i = bitcount - bits_available; i > 0; i--) {
			transmit_assert(D0_SENSE);
		}
	}
	for (uint32_t i = 0; i < sendValue.length(); i++) {
		byte c = char_to_byte(sendValue.charAt(i));
		DEBUGF("i: %u  BYTE:0x%u\n", i , c);
		for (short x = 3 - excess_bits; x >= 0; x--) {
			//DEBUGLN("x:" + String(x) + " b:" + b);
			if (bitRead(c, x)) {
				transmit_assert(D1_SENSE);
			}
			else {
				transmit_assert(D0_SENSE);
			}
		}
		excess_bits = 0;
	}
}

void append_log(String text) {
	File file = SPIFFS.open("/log.txt", "a");
	if (file) {
		file.println(String(millis()) + ' ' + text);
		DEBUGLN("Appending to log: " + String(millis()) + ' ' + text);
		file.close();
	}
	else DEBUGLN("Failed opening log file.");
}

void syslog(String text) {
	if (WiFi.status() != WL_CONNECTED || syslog_server == INADDR_NONE) return;
	char buf[101];
	text = '<' + String(syslog_priority) + '>' + String(syslog_host) + ' ' + String(syslog_service_name) + ": " + text;
	text.toCharArray(buf, sizeof(buf) - 1);
	Udp.beginPacket(syslog_server, syslog_port);
	Udp.write(buf);
	Udp.endPacket();
}

bool basicAuthFailed() {
	if (strlen(www_username) > 0 && strlen(www_password) > 0) {
		if (!server.authenticate(www_username, www_password)) {
			server.requestAuthentication();
			return true;
		}
	}
	return false;    // This is good
}

void handleFileDelete() {
	if (basicAuthFailed()) return;
	if (server.args() == 0) return server.send(500, F("text/plain"), F("BAD ARGS"));
	String path = server.arg(0);
	DEBUGLN("handleFileDelete: " + path);
	if (path == "/")
		return server.send(500, F("text/plain"), F("BAD PATH"));
	if (!SPIFFS.exists(path))
		return server.send(404, F("text/plain"), F("FileNotFound"));
	SPIFFS.remove(path);
	server.send(200, F("text/plain"), "");
}

void handleTxId() {
	if (!server.hasArg("v")) { server.send(500, F("text/plain"), F("BAD ARGS")); return; }
	String value = server.arg("v");
	//DEBUGLN("handleTxId: " + value);
	String sendValue = value.substring(0, value.indexOf(":"));
	sendValue.toUpperCase();
	uint32_t bitcount = value.substring(value.indexOf(":") + 1).toInt();
	transmit_id(sendValue, bitcount);
	server.send(200, F("text/plain"), "");
}

bool loadConfig() {
	using namespace su;
	File configFile = SPIFFS.open(CONFIG_FILE, "r");
	if (!configFile) {
		DEBUGLN(F("Failed to open config file"));
		return false;
	}

	size_t size = configFile.size();
	if (size > 1024) {
		DEBUGLN(F("Config file size is too large"));
		return false;
	}
	// Allocate a buffer to store contents of the file.
	std::unique_ptr<char[]> buf(new char[size]);
	// We don't use String here because ArduinoJson library requires the input
	// buffer to be mutable. If you don't use ArduinoJson, you may as well
	// use configFile.readString instead.
	configFile.readBytes(buf.get(), size);
	gson::Parser json;
	json.parse(buf.get());
	json.hashKeys();
	//StaticJsonBuffer<1024> jsonBuffer;
	//JsonObject& json = jsonBuffer.parseObject(buf.get());
	DEBUG(F("Parse config file "));
	if (json.hasError()) {
		DEBUGF("failed, %s in %u\n",json.readError(), json.errorIndex());
		return false;
	}else DEBUGLN(F("done"));

	// FIXME these should be testing for valid input before replacing defaults
	if (json.has(SH("log_name"))) {
		strncpy(log_name, json[SH("log_name")].str(), sizeof(log_name));
		DEBUGLN("Loaded log_name: " + String(log_name));
	}
	if (json.has(SH("ap_enable"))) {
		ap_enable = (strcmp(json[SH("ap_enable")].str(), "true") == 0) ? true : false;
		DEBUGLN("Loaded ap_enable: " + String(ap_enable));
	}
	if (json.has(SH("ap_hidden"))) {
		ap_hidden = (strcmp(json[SH("ap_hidden")].str(), "true") == 0) ? true : false;
		DEBUGLN("Loaded ap_hidden: " + String(ap_hidden));
	}
	if (json.has(SH("ap_ssid"))) {
		strncpy(ap_ssid, json[SH("ap_ssid")].str(), 20);
		DEBUGLN("Loaded ap_ssid: " + String(ap_ssid));
	}
	if (json.has(SH("ap_psk"))) {
		strncpy(ap_psk, json[SH("ap_psk")].str(), sizeof(ap_psk));
		DEBUGLN("Loaded ap_psk: " + String(ap_psk));
	}
	if (json.has(SH("station_ssid"))) {
		strncpy(station_ssid, json[SH("station_ssid")].str(), sizeof(station_ssid));
		DEBUGLN("Loaded station_ssid: " + String(station_ssid));
	}
	if (json.has(SH("station_psk"))) {
		strncpy(station_psk, json[SH("station_psk")].str(), sizeof(station_psk));
		DEBUGLN("Loaded station_psk: " + String(station_psk));
	}
	if (json.has(SH("mDNShost"))) {
		strncpy(mDNShost, json[SH("mDNShost")].str(), sizeof(mDNShost));
		DEBUGLN("Loaded mDNShost: " + String(mDNShost));
	}
	if (json.has(SH("DoS_id"))) {
		DoS_id = json[SH("DoS_id")].str();
		DEBUGLN("Loaded DoS_id: " + DoS_id);
	}
	if (json.has(SH("ota_password"))) {
		strncpy(ota_password, json[SH("ota_password")].str(), sizeof(ota_password));
		DEBUGLN("Loaded ota_password: " + String(ota_password));
	}
	if (json.has(SH("www_username"))) {
		strncpy(www_username, json[SH("www_username")].str(), sizeof(www_username));
		DEBUGLN("Loaded www_username: " + String(www_username));
	}
	if (json.has(SH("www_password"))) {
		strncpy(www_password, json[SH("www_password")].str(), sizeof(www_password));
		DEBUGLN("Loaded www_password: " + String(www_password));
	}
	if (json.has(SH("syslog_server"))) {
		syslog_server.fromString(json[SH("syslog_server")]);
		DEBUGLN("Loaded syslog_server: " + syslog_server.toString());
	}
	if (json.has(SH("syslog_port"))) {
		syslog_port = json[SH("syslog_port")];
		DEBUGLN("Loaded syslog_port: " + String(syslog_port));
	}
	if (json.has(SH("syslog_service_name"))) {
		strncpy(syslog_service_name, json[SH("syslog_service_name")].str(), sizeof(syslog_service_name));
		DEBUGLN("Loaded syslog_service_name: " + String(syslog_service_name));
	}
	if (json.has(SH("syslog_host"))) {
		strncpy(syslog_host, json[SH("syslog_host")].str(), sizeof(syslog_host));
		DEBUGLN("Loaded syslog_host: " + String(syslog_host));
	}
	if (json.has(SH("syslog_priority"))) {
		syslog_priority = json[SH("syslog_priority")];
		DEBUGLN("Loaded syslog_priority: " + String(syslog_priority));
	}
	return true;
}

//format bytes
String formatBytes(size_t bytes) {
	String ret;
	if (bytes < 0x400) { ret = bytes; ret += 'B'; }
	else if (bytes < 0x100000) { ret = (bytes / 1024.0f); ret += "KB"; }
	else if (bytes < 0x40000000) { ret = bytes / 1048576.0f; ret += "MB"; }
	else { ret = bytes / 1073741824.0f; ret += "GB"; }
	return ret;
}

String getContentType(String& filename) {
	if (server.hasArg("download")) return F("application/octet-stream");
	else if (filename.endsWith(".htm")) return F("text/html");
	else if (filename.endsWith(".html")) return F("text/html");
	else if (filename.endsWith(".css")) return F("text/css");
	else if (filename.endsWith(".js")) return F("application/javascript");
	else if (filename.endsWith(".json")) return F("text/json");
	else if (filename.endsWith(".png")) return F("image/png");
	else if (filename.endsWith(".gif")) return F("image/gif");
	else if (filename.endsWith(".jpg")) return F("image/jpeg");
	else if (filename.endsWith(".ico")) return F("image/x-icon");
	else if (filename.endsWith(".svg")) return F("image/svg+xml");
	else if (filename.endsWith(".xml")) return F("text/xml");
	else if (filename.endsWith(".pdf")) return F("application/x-pdf");
	else if (filename.endsWith(".zip")) return F("application/x-zip");
	else if (filename.endsWith(".gz")) return F("application/x-gzip");
	return "text/plain";
}

bool handleFileRead(String path) {
	DEBUGLN("handleFileRead: " + path);
	if (path.equals(F("/"))) path = F("/static/index.htm");
	if (path.endsWith(F("/"))) path += F("index.htm");
	String contentType = getContentType(path);
	String pathWithGz = path + F(".gz");
	if (SPIFFS.exists(pathWithGz) || SPIFFS.exists(path)) {
		if (SPIFFS.exists(pathWithGz))
			path += F(".gz");
		File file = SPIFFS.open(path, "r");
		server.sendHeader("Now", String(millis()));
		size_t sent = server.streamFile(file, contentType);
		file.close();
		return true;
	}
	return false;
}

void handleFileUpload() {
	if (basicAuthFailed()) return;
	if (server.uri() != F("/edit")) return;
	HTTPUpload& upload = server.upload();
	if (upload.status == UPLOAD_FILE_START) {
		String filename = upload.filename;
		if (!filename.startsWith("/")) filename = "/" + filename;
		DEBUGLN("handleFileUpload Name: " + filename);
		fsUploadFile = SPIFFS.open(filename, "w");
	}
	else if (upload.status == UPLOAD_FILE_WRITE) {
		//DEBUGLN("handleFileUpload Data: " + upload.currentSize);
		if (fsUploadFile)
			fsUploadFile.write(upload.buf, upload.currentSize);
	}
	else if (upload.status == UPLOAD_FILE_END) {
		if (fsUploadFile)
			fsUploadFile.close();
		DEBUGLN("handleFileUpload Size: " + upload.totalSize);
	}
}

void handleFileCreate() {
	if (basicAuthFailed()) return;
	if (server.args() == 0)
		return server.send(500, "text/plain", "BAD ARGS");
	String path = server.arg(0);
	DEBUGLN("handleFileCreate: " + path);
	if (path == "/")
		return server.send(500, "text/plain", "BAD PATH");
	if (SPIFFS.exists(path))
		return server.send(500, "text/plain", "FILE EXISTS");
	File file = SPIFFS.open(path, "w");
	if (file)
		file.close();
	else
		return server.send(500, "text/plain", "CREATE FAILED");
	server.send(200, "text/plain", "");
}

void handleFileList() {
	if (basicAuthFailed()) return;
	if (!server.hasArg("dir")) { server.send(500, "text/plain", "BAD ARGS"); return; }

	String path = server.arg("dir");
	DEBUGLN("handleFileList: " + path);
	Dir dir = SPIFFS.openDir(path);
	//path = String();

	String output = "[";
	while (dir.next()) {
		File entry = dir.openFile("r");
		if (output != "[") output += ',';
		bool isDir = false;
		output += "{\"type\":\"";
		output += (isDir) ? "dir" : "file";
		output += "\",\"name\":\"";
		output += entry.name();//String(entry.name()).substring(1);
		output += "\"}";
		entry.close();
	}
	output += "]";
	server.send(200, "text/json", output);
}

void handleDoS() {
	if (basicAuthFailed()) return;
	digitalWrite(D0_SENSE, HIGH);
	server.send(200, F("text/plain"), "");
	append_log(F("DoS mode set by API request."));
}

void handleRestart() {
	if (basicAuthFailed()) return;
	append_log(F("Restart requested by user."));
	server.send(200, "text/plain", "OK");
	ESP.restart();
}

void resetConfig(void) {
	static bool reset_pin_state = true;
	if (millis() > 30000) return;
	if (!digitalRead(CONF_RESET) && reset_pin_state) {
		reset_pin_state = false;
		config_reset_millis = millis();
	}
	else {
		reset_pin_state = true;
		if (millis() > (config_reset_millis + 2000)) {
			append_log(F("Config reset by pin."));
			SPIFFS.remove(CONFIG_FILE);
			ESP.restart();
		}
	}
}

void auxChange(void) {
	byte new_value = digitalRead(LED_SENSE);
	if (new_value == expect_aux) {
		last_aux = new_value;
		expect_aux = 2;
		return;
	}
	if (new_value != last_aux) {
		if (millis() > aux_change) {
			aux_change = millis() + 10;
			last_aux = new_value;
			append_log("Aux changed to " + String(new_value));
		}
	}
}

String grep_auth_file() {
	char buffer[64];
	char* this_id;
	byte cnt = 0;

	File file = SPIFFS.open(AUTH_FILE, "r");
	if (!file) {
		DEBUGLN(F("Failed to open auth file"));
		return "";
	}

	while (file.available() > 0) {
		char c = file.read();
		buffer[cnt++] = c;
		if ((c == '\n') || (cnt == sizeof(buffer) - 1)) {
			buffer[cnt] = '\0';
			cnt = 0;
			this_id = strtok(buffer, " ");
			if (reader1_string == String(this_id)) {
				return String(strtok(NULL, "\n"));
			}
		}
	}
	return "";
}

void server_init() {
	server.on("/dos", HTTP_GET, handleDoS);
	server.on("/txid", HTTP_GET, handleTxId);
	server.on("/format", HTTP_DELETE, []() {
		if (basicAuthFailed());
		if (SPIFFS.format()) server.send(200, "text/plain", "Format success!");
		});
	//list directory
	server.on("/list", HTTP_GET, handleFileList);
	//load editor
	server.on("/edit", HTTP_GET, []() {
		if (basicAuthFailed());
		if (!handleFileRead("/static/edit.htm")) server.send(404, "text/plain", "FileNotFound");
		});
	//create file
	server.on("/edit", HTTP_PUT, handleFileCreate);
	//delete file
	server.on("/edit", HTTP_DELETE, handleFileDelete);
	//first callback is called after the request has ended with all parsed arguments
	//second callback handles file uploads at that location
	server.on("/edit", HTTP_POST, []() { server.send(200, "text/plain", ""); }, handleFileUpload);
	server.on("/restart", HTTP_GET, handleRestart);
	//called when the url is not defined here
	//use it to load content from SPIFFS
	server.onNotFound([]() {
		if (basicAuthFailed()) return;
		if (!handleFileRead(server.uri()))
			server.send(404, "text/plain", "FileNotFound");
		});

	server.on("/version", HTTP_GET, []() {
		if (basicAuthFailed()) return;
		String json = "{\"version\":\"" + String(VERSION) + "\",\"log_name\":\"" + String(log_name) + "\",\"ChipID\":\"" + String(ESP.getChipId(), HEX) + "\"}\n";
		server.send(200, "text/json", json);
		json = String();
		});
	//get heap status, analog input value and all GPIO statuses in one json call
	server.on("/all", HTTP_GET, []() {
		if (basicAuthFailed()) return;
		String json = "{";
		json += "\"heap\":" + String(ESP.getFreeHeap());
		json += ", \"analog\":" + String(analogRead(A0));
		json += ", \"gpio\":" + String((uint32_t)(((GPI | GPO) & 0xFFFF) | ((GP16I & 0x01) << 16)));
		json += "}";
		server.send(200, "text/json", json);
		json = String();
		});
	server.serveStatic("/static", SPIFFS, "/static", "max-age=86400");
	httpUpdater.setup(&server);	// This doesn't do authentication
	server.begin();
	MDNS.addService("http", "tcp", 80);
	DEBUGLN(F("HTTP server started"));
}
