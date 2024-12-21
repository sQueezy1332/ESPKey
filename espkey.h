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

#include <WiFiUdp.h>
WiFiUDP Udp;
#ifdef ESP8266
#define uS micros64()
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ESP8266HTTPUpdateServer.h>
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;
#else
#define uS esp_timer_get_time()
#include <WiFiServer.h>
#include <HTTPUpdateServer.h>
WiFiServer server(80);
HTTPUpdateServer httpUpdater;
#include <SPIFFS.h>
#endif
#include <FS.h>
#include <ArduinoOTA.h>
#include <GSON.h>
#include <StringUtils.h>
#include <GyverIO.h>
#include <OneWireSlave.h>

#define VERSION "131"
#define HOSTNAME "ESPKey" // Hostname prefix for DHCP/OTA
#define CONFIG_FILE "/config.json"
#define AUTH_FILE "/auth.txt"
#define CARD_LEN 4     // minimum card length in bits
#define PULSE_WIDTH 100 // length of asserted pulse in microSeconds
#define PULSE_DELTA 2000   // delay between pulses in microSeconds

// Pin number assignments
#define PIN_D0 4
#define PIN_D1 5
#define PIN_LED 2
#define PIN_MODE 16
#define PIN_CONF_RESET 14
#define LED 2

#define dWrite(pin, x) gio::write(pin, x)
#define dRead(pin) gio::read(pin)

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

#if defined CONFIG_IDF_TARGET_ESP32C3 || defined ESP8266
#define LED_ON	0
#define LED_OFF	1
#else
#define LED_ON	1
#define LED_OFF	0
#endif
#define LED_DELAY 100
#define LED_BLINK_COUNT 5

// Default settings used when no configuration file exists
String log_name("Alpha");
String ap_ssid(HOSTNAME); // Default SSID.
IPAddress ap_ip(192, 168, 4, 1);
String ap_psk("defaultpass"); // Default PSK.
String station_ssid;
String station_psk;
String mDNShost("espkey");
String DoS_id("1ffffff:26");
String ota_password("ExtraSpecialPassKey");
String www_username;
String www_password;
String syslog_service_name("accesscontrol");
String syslog_host(HOSTNAME);
IPAddress syslog_server(0, 0, 0, 0);
uint16_t syslog_port = 514;
bool ap_enable = true;
bool ap_hidden = false;
byte syslog_priority = 36;

const byte pin_onewire = PIN_D0;
volatile bool presence_flag = false;
byte onewire_mode = 0;
OneWireSniffer slave;

String reader_string;
volatile uint64_t reader_code = 0;
volatile byte reader_count = 0;
volatile decltype(uS) reader_last = 0;
volatile uint32_t reader_delta = 0;

/*
volatile byte last_aux = 1;
volatile byte expect_aux = 2;
volatile uint32_t aux_change = 0;
*/
void IRAM_ATTR reader1_D0_trigger(void) {
	auto time = uS;
	if (reader_last) reader_delta += time - reader_last;
	reader_last = time;
	reader_count++;
	reader_code <<= 1;
}

void IRAM_ATTR reader1_D1_trigger(void) {
	reader1_D0_trigger();
	reader_code |= 1;
}

void IRAM_ATTR onewire_presence() {
	auto time = uS;
	uint32_t delta = time - reader_last;
	if (onewire_mode == RISING) { goto  _presence; }
	if (delta < (480 + 80) && delta > 480) {
_presence:
		presence_flag = true;
		ETS_GPIO_INTR_DISABLE();
	} reader_last = time;
}

bool onewire_handle() {
	byte cmd;
	presence_flag = false;
	if (onewire_mode == RISING) { 
		if (!dRead(pin_onewire)) {
			if(!slave.waitReset(1) || !slave.presenceDetection()) goto _exit;
		}
	};
	if (!slave.recvAndProcessCmd((byte*)&reader_code, cmd)) goto _exit;
	reader_count = cmd;
	delay(5);
_exit:
	ETS_GPIO_INTR_ENABLE();
	return slave.error == slave.ONEWIRE_NO_ERROR;
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

void led_detach() {
#ifdef ESP8266
	Dir root = SPIFFS.openDir("/");
#else
	File root = SPIFFS.open("/");
#endif
	if (root.isDirectory()) {
		String fileName;
		size_t fileSize;
#ifdef ESP8266
		while (root.next()) {
			fileName = root.fileName();
			fileSize = root.fileSize();
#else
		for (File file = root.openNextFile(); file; file = root.openNextFile()) {
			if (file.isDirectory()) continue;
			fileName = file.name();
			fileSize = file.fileSize();

#endif // ESP8266
					// This is a dirty hack to deal with readers which don't pull LED up to 5V
			if (fileName == "/auth.txt") detachInterrupt((digitalPinToInterrupt(PIN_LED)));
			DEBUGF("FS File: %s, size: %u bytes\n", fileName.c_str(), fileSize/*formatBytes(file.fileSize()).c_str()*/);

		}
	}
}

void reader_reset() {
	reader_code = 0;
	reader_count = 0;
	reader_last = 0;
	reader_delta = 0;
	//reader_string = "";
}

void transmit_assert(bool bit, uint16_t pulse_delta = PULSE_DELTA) {
	bit ? dWrite(PIN_D1, LOW) : dWrite(PIN_D0, LOW);
	delayMicroseconds(PULSE_WIDTH);
	bit ? dWrite(PIN_D1, HIGH) : dWrite(PIN_D0, HIGH);
	delayMicroseconds(pulse_delta - PULSE_WIDTH);
}

void transmit_id_nope(uint64_t sendValue, byte bitcount, uint16_t pulse_delta = PULSE_DELTA) {
	DEBUGLN("[-] Sending Data: " + String(sendValue, HEX) + ':' + String(bitcount) + '\n');
	yield();
	noInterrupts();
	for (uint64_t bitmask = 1 << (bitcount - 1); bitmask; bitmask >>= 1) {
		transmit_assert(sendValue & bitmask, pulse_delta);
	}
	interrupts();
	yield();

}

void transmit_id(String sendValue, uint32_t bitcount) {
	DEBUGLN("Sending data: " + sendValue + ':' + bitcount);
	uint32_t bits_available = sendValue.length() << 2; //* 4
	uint32_t excess_bits = 0;
	if (bits_available > bitcount) {
		excess_bits = bits_available - bitcount;
		sendValue = sendValue.substring(excess_bits >> 2); // / 4
		excess_bits &= 3; //%= 4;
		DEBUG("sending: " + sendValue + " with excess bits: " + excess_bits + "\n\t");
	} else if (bits_available < bitcount) {
		for (uint32_t i = bitcount - bits_available; i > 0; i--) {
			transmit_assert(PIN_D0);
		}
	}
	for (uint32_t i = 0; i < sendValue.length(); i++) {
		byte BYTE = char_to_byte(sendValue.charAt(i));
		DEBUGF("i: %u  BYTE:0x%u\n", i, BYTE);
		for (short x = 3 - excess_bits; x >= 0; x--) {
			//DEBUGLN("x:" + String(x) + " b:" + b);
			if (bitRead(BYTE, x)) transmit_assert(PIN_D1);
			else transmit_assert(PIN_D0);
		}
		excess_bits = 0;
	}
}

void append_log(const String & text) {
	File file = SPIFFS.open("/log.txt", "a");
	if (file) {
		String log(systimer / 1000); log += ' '; log += text;
		file.println(log);
		DEBUGLN("Appending to log: " + log);
		file.close();
	} else DEBUGLN("Failed opening log file.");
}

void syslog(String text) {
	if (WiFi.status() != WL_CONNECTED || syslog_server == INADDR_NONE) return;
	String log('<');
	log += syslog_priority; log += '>'; log += syslog_host; log += ' '; log += syslog_service_name; log += ": "; log += text;
	Udp.beginPacket(syslog_server, syslog_port);
	Udp.write(log.c_str(), log.length());
	Udp.endPacket();
}

void IRAM_ATTR resetConfig(void) {
	static bool reset_pin_state = true;
	auto config_reset_millis = uS;
	if (millis() > 30000) return;
	if (!digitalRead(PIN_CONF_RESET) && reset_pin_state) {
		reset_pin_state = false;
		config_reset_millis = millis();
	} else {
		reset_pin_state = true;
		if (millis() - config_reset_millis > 2000) {
			append_log(F("Config reset by pin."));
			SPIFFS.remove(CONFIG_FILE);
			ESP.restart();
		}
	}
}

bool basicAuthFailed() {
	if (www_username.length() && www_password.length()) {
		if (!server.authenticate(www_username.c_str(), www_password.c_str())) {
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
	const String& value = server.arg("v");
	DEBUGLN("handleTxId: " + value);
	const char* str = value.c_str();
	char* colon = strchr(str, ':');
	uint64_t sendValue = strtoull(str, &colon, HEX); DEBUGF("sendValue %X\n", sendValue);
	String result;
	if (onewire_mode) {
		OneWireSlave emul;
		emul.init((byte*)&sendValue);
		auto timestamp = uS + 1000000;
		for (byte i = 0; i < 23; i++) {
			while (uS < timestamp) {
				if (!emul.waitReset(1) && emul.error != emul.ONEWIRE_WAIT_RESET_TIMEOUT) continue;
				if (!emul.presence()) continue;
				if (!emul.recvAndProcessCmd()) continue;
				break;
			}
		}
		result = String(emul.error);
	} else {
		byte bitcount = strtoul(colon + 1, &colon, 10);	DEBUGF("bitcount %u\n", bitcount);
		uint32_t pulse_delta = strtoul(colon + 1, NULL, DEC); DEBUGF("pulse_delta %u\n", pulse_delta);
		transmit_id_nope(sendValue, bitcount, pulse_delta > INT16_MAX ? 2000 : pulse_delta);
	}
	server.send(200, F("text/plain"), result);
}

bool loadConfig() {
	using namespace su;
	File configFile = SPIFFS.open(CONFIG_FILE, "r");
	if (!configFile) {
		DEBUGLN(F("Failed to open config file"));
		return false;
	}

	size_t size = configFile.size();
	DEBUGF("File size %lu\n", size);
	if (size > 0x400) {
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
		DEBUGF("failed, %s in %u\n", json.readError(), json.errorIndex());
		//return false;
	} else DEBUGLN(F("done"));

	// FIXME these should be testing for valid input before replacing defaults
	if (json.has(SH("log_name"))) {
		json[SH("log_name")].value().toString(log_name);
		DEBUGLN("Loaded log_name: " + log_name);
	}
	if (json.has(SH("ap_enable"))) {
		ap_enable = json[SH("ap_enable")].value().toBool();
		DEBUGLN("Loaded ap_enable: " + String(ap_enable));
	}
	if (json.has(SH("ap_hidden"))) {
		ap_hidden = json[SH("ap_hidden")].value().toBool();
		DEBUGLN("Loaded ap_hidden: " + String(ap_hidden));
	}
	if (json.has(SH("ap_ssid"))) {
		json[SH("ap_ssid")].value().toString(ap_ssid);
		DEBUGLN("Loaded ap_ssid: " + ap_ssid);
	}
	if (json.has(SH("ap_psk"))) {
		json[SH("ap_psk")].value().toString(ap_psk);
		DEBUGLN("Loaded ap_psk: " + ap_psk);
	}
	if (json.has(SH("station_ssid"))) {
		json[SH("station_ssid")].value().toString(station_ssid);
		DEBUGLN("Loaded station_ssid: " + station_ssid);
	}
	if (json.has(SH("station_psk"))) {
		json[SH("station_psk")].value().toString(station_psk);
		DEBUGLN("Loaded station_psk: " + station_psk);
	}
	if (json.has(SH("mDNShost"))) {
		json[SH("mDNShost")].value().toString(mDNShost);
		DEBUGLN("Loaded mDNShost: " + mDNShost);
	}
	if (json.has(SH("DoS_id"))) {
		json[SH("DoS_id")].value().toString(DoS_id);
		DEBUGLN("Loaded DoS_id: " + DoS_id);
	}
	if (json.has(SH("ota_password"))) {
		json[SH("ota_password")].value().toString(ota_password);
		DEBUGLN("Loaded ota_password: " + ota_password);
	}
	if (json.has(SH("www_username"))) {
		json[SH("www_username")].value().toString(www_username);
		DEBUGLN("Loaded www_username: " + www_username);
	}
	if (json.has(SH("www_password"))) {
		json[SH("www_password")].value().toString(www_password);
		DEBUGLN("Loaded www_password: " + www_password);
	}
	if (json.has(SH("syslog_server"))) {
		syslog_server.fromString(json[SH("syslog_server")].value());
		DEBUGLN("Loaded syslog_server: " + syslog_server.toString());
	}
	if (json.has(SH("syslog_port"))) {
		syslog_port = json[SH("syslog_port")].value();
		DEBUGLN("Loaded syslog_port: " + String(syslog_port));
	}
	if (json.has(SH("syslog_service_name"))) {
		json[SH("syslog_service_name")].value().toString(syslog_service_name);
		DEBUGLN("Loaded syslog_service_name: " + String(syslog_service_name));
	}
	if (json.has(SH("syslog_host"))) {
		json[SH("syslog_host")].value().toString(syslog_host);
		DEBUGLN("Loaded syslog_host: " + String(syslog_host));
	}
	if (json.has(SH("syslog_priority"))) {
		syslog_priority = json[SH("syslog_priority")].value();
		DEBUGLN("Loaded syslog_priority: " + String(syslog_priority));
	}

	return true;
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
	if (basicAuthFailed() || server.uri() != F("/edit")) return;
	File fsUploadFile;
	HTTPUpload& upload = server.upload();
	if (upload.status == UPLOAD_FILE_START) {
		String filename = upload.filename;
		if (!filename.startsWith("/")) filename = "/" + filename;
		DEBUGLN("handleFileUpload Name: " + filename);
		fsUploadFile = SPIFFS.open(filename, "w");
		//filename = String();
	} else if (upload.status == UPLOAD_FILE_WRITE) {
		DEBUGLN("handleFileUpload Data: " + upload.currentSize);
		if (fsUploadFile)
			fsUploadFile.write(upload.buf, upload.currentSize);
	} else if (upload.status == UPLOAD_FILE_END) {
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
	String output = "[";
#ifdef ESP8266
	Dir dir = SPIFFS.openDir(path);
	File entry;
	while (dir.next()) {
#else
	File dir = SPIFFS.open(path);
	for (File entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
#endif // ESP8266
		if (output != "[") output += ',';
		output += "{\"type\":\"";
		output += entry.isDirectory() ? "dir" : "file";
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
	dWrite(PIN_D0, LOW);
	server.send(200, F("text/plain"), "");
	append_log(F("DoS mode set by API request."));
}

void handleRestart() {
	if (basicAuthFailed()) return;
	append_log(F("Restart requested by user."));
	server.send(200, "text/plain", "OK");
	ESP.restart();
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
	//use it to load result from SPIFFS
	server.onNotFound([]() {
		if (basicAuthFailed()) return;
		if (!handleFileRead(server.uri()))
			server.send(404, "text/plain", "FileNotFound");
		});
	server.on("/version", HTTP_GET, []() {
		if (basicAuthFailed()) return;
		String json = "{\"version\":\"" + String(VERSION) + "\",\"log_name\":\"" + String(log_name) + "\",\"ChipID\":\"" + String(ESP.
#ifdef ESP8266
			getChipId()
#else
			getChipModel()
#endif

			, HEX) + "\"}\n";
		server.send(200, "text/json", json);
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
		});
	server.serveStatic("/static", SPIFFS, "/static", "max-age=86400");
	httpUpdater.setup(&server);	// This doesn't do authentication
	server.begin();
	MDNS.addService("http", "tcp", 80);
	DEBUGLN(F("HTTP server started"));
}

bool wifi_sta_init(bool wait = false) {
	WiFi.mode(WIFI_STA);
#ifdef DEBUG_ENABLE
	WiFi.printDiag(Serial);
#endif
	// ... Compare file config with sdk config.
	if (WiFi.SSID() != station_ssid || WiFi.psk() != station_psk) {
		DEBUGLN(F("WiFi config changed.  Attempting new connection"));
		WiFi.begin(station_ssid, station_psk);// ... Try to connect as WiFi station.
		DEBUGLN("new SSID: " + String(WiFi.SSID()));
	} else WiFi.begin();// ... Begin with sdk config.

	DEBUGLN(F("Wait for WiFi connection."));
	// ... Give ESP 10 seconds to connect to station.
	if (wait) {
		for (auto timestamp = millis() + 10000; WiFi.status() != WL_CONNECTED && millis() < timestamp;) {
			DEBUG(". ");//DEBUG(WiFi.status());
			delay(500);
		} DEBUGLN();
	}
	// Check connection
	if (WiFi.status() == WL_CONNECTED) {
		DEBUG("IP address: "); DEBUGLN(WiFi.localIP());
		return true;
	}
	return false;
}

//format bytes
String formatBytes(size_t bytes) {
	String ret;
	if (bytes < 0x400) { ret = bytes; ret += 'B'; } else if (bytes < 0x100000) { ret = (bytes / 1024.0f); ret += "KB"; } else if (bytes < 0x40000000) { ret = bytes / 1048576.0f; ret += "MB"; } else { ret = bytes / 1073741824.0f; ret += "GB"; }
	return ret;
}

void fix_reader_string() {
	byte loose_bits = reader_count & 3; //%4
	if (loose_bits) {
		byte moving_bits = 4 - loose_bits;
		byte moving_mask = (1 << moving_bits) - 1;
		DEBUGLN("lb: " + String(loose_bits) + " mb: " + String(moving_bits) + " mm: " + String(moving_mask, HEX));
		byte BYTE = char_to_byte(reader_string[0]);
		uint32_t str_len = reader_string.length();
		for (uint32_t i = 0; i < str_len; i++) {
			reader_string[i] = c2h(BYTE >> moving_bits);
			BYTE &= moving_mask;
			BYTE = (BYTE << 4) | char_to_byte(reader_string.charAt(i + 1));
			DEBUGLN("BYTE: " + String(BYTE, HEX) + " i: " + String(reader_string.charAt(i)));
		}
		reader_string += String((BYTE >> moving_bits) | reader_code, HEX);
	}
	reader_string += ':' + String(reader_count);
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
			if (reader_string == String(this_id)) {
				return String(strtok(NULL, "\n"));
			}
		}
	}
	return "";
}

void led_blink(byte count = LED_BLINK_COUNT, byte led_delay = LED_DELAY) {
	while (count--) {
		dWrite(LED, LED_ON);
		delay(led_delay);
		dWrite(LED, LED_OFF);
		delay(led_delay);
	}
}
/*
void auxChange(void) {
	byte new_value = digitalRead(PIN_LED);
	if (new_value == expect_aux) {
		last_aux = new_value;
		expect_aux = 2;
		return;
	}
	if (new_value != last_aux) {
		if (millis() > aux_change) {
			aux_change = millis() + 10;
			last_aux = new_value;
			append_log("Aux " + String(new_value));
		}
	}
}

byte toggle_pin(byte pin) {
	byte new_value = !digitalRead(pin);
	if (pin == PIN_LED) expect_aux = !new_value;
	dWrite(pin, new_value);
	return new_value;
}
*/


