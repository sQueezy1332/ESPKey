#include "espkey.h"

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

void setup() {
	//// Outputs
	//pinMode(D0_ASSERT, OUTPUT);
	//digitalWrite(D0_ASSERT, LOW);
	//pinMode(D1_ASSERT, OUTPUT);
	//digitalWrite(D1_ASSERT, LOW);
	//pinMode(LED_ASSERT, OUTPUT);
	//digitalWrite(LED_ASSERT, LOW);
	// Inputs
	pinMode(D0_SENSE, OUTPUT_OPEN_DRAIN);
	pinMode(D1_SENSE, OUTPUT_OPEN_DRAIN);
	pinMode(LED_SENSE, OUTPUT_OPEN_DRAIN);
	pinMode(CONF_RESET, OUTPUT_OPEN_DRAIN);

	// Input interrupts
	attachInterrupt(digitalPinToInterrupt(D0_SENSE), reader1_D0_trigger, FALLING);
	attachInterrupt(digitalPinToInterrupt(D1_SENSE), reader1_D1_trigger, FALLING);
	attachInterrupt(digitalPinToInterrupt(LED_SENSE), auxChange, CHANGE);
	attachInterrupt(digitalPinToInterrupt(CONF_RESET), resetConfig, CHANGE);
#ifdef DEBUG_ENABLE
	Serial.begin(115200);
	delay(100);
	Serial.setDebugOutput(true);
	DEBUGLN("Chip ID: 0x" + String(ESP.getChipId(), HEX));
#endif // DEBUG_ENABLE
	// Set Hostname.
	String dhcp_hostname(HOSTNAME);
	dhcp_hostname += String(ESP.getChipId(), HEX);
	WiFi.hostname(dhcp_hostname);

	// Print hostname.
	DEBUGLN("Hostname: " + dhcp_hostname);

	if (!SPIFFS.begin()) {
		DEBUGLN(F("Failed to mount file system"));
		return;
	}
	else {
		Dir dir = SPIFFS.openDir("/");
		while (dir.next()) {
			String fileName = dir.fileName();
			// This is a dirty hack to deal with readers which don't pull LED up to 5V
			if (fileName == String("/auth.txt")) detachInterrupt(digitalPinToInterrupt(LED_SENSE));

			size_t fileSize = dir.fileSize();
			DEBUGF("FS File: %s, size: %s\n", fileName.c_str(), formatBytes(fileSize).c_str());
		}
	}
	append_log(F("Starting up!"));

	// If a log.txt exists, use ap_ssid=ESPKey-<chipid> instead of the default ESPKey-config
	// A config file will take precedence over this
	if (SPIFFS.exists("/log.txt")) dhcp_hostname.toCharArray(ap_ssid, sizeof(ap_ssid));

	// Load config file.
	if (!loadConfig()) {
		DEBUGLN(F("No configuration information available.  Using defaults."));
	}

	// Check WiFi connection
	// ... check mode
	if (WiFi.getMode() != WIFI_STA) {
		WiFi.mode(WIFI_STA);
		delay(10);
	}

	// ... Compare file config with sdk config.
	if (WiFi.SSID() != station_ssid || WiFi.psk() != station_psk) {
		DEBUGLN(F("WiFi config changed.  Attempting new connection"));

		// ... Try to connect as WiFi station.
		WiFi.begin(station_ssid, station_psk);

		DEBUGLN("new SSID: " + String(WiFi.SSID()));

		// ... Uncomment this for debugging output.
		//WiFi.printDiag(Serial);
	}
	else {
		// ... Begin with sdk config.
		WiFi.begin();
	}

	DEBUGLN(F("Wait for WiFi connection."));

	// ... Give ESP 10 seconds to connect to station.
	uint32_t startTime = millis();
	while (WiFi.status() != WL_CONNECTED && millis() - startTime < 10000) {
		DEBUG(". ");
		//DEBUG(WiFi.status());
		delay(500);
	}
	DEBUGLN();

	// Check connection
	if (WiFi.status() == WL_CONNECTED) {
		// ... print IP Address
		DEBUG("IP address: ");
		DEBUGLN(WiFi.localIP());
	}
	else if (ap_enable) {
			DEBUGLN(F("Can not connect to WiFi station. Going into AP mode."));

			// Go into software AP mode.
			WiFi.mode(WIFI_AP);
			WiFi.softAPConfig(ap_ip, ap_ip, IPAddress(255, 255, 255, 0));
			WiFi.softAP(ap_ssid, ap_psk, 0, ap_hidden);

			DEBUG(F("IP address: "));
			DEBUGLN(WiFi.softAPIP());
		}
		else {
			DEBUGLN(F("Can not connect to WiFi station. Bummer."));
			//WiFi.mode(WIFI_OFF);
		}
	// Start OTA server.
	ArduinoOTA.setHostname(dhcp_hostname.c_str());
	ArduinoOTA.setPassword(ota_password);
	ArduinoOTA.begin();
	if (MDNS.begin(mDNShost)) {
		DEBUGLN("Open http://" + String(mDNShost) + ".local/");
	}
	else { DEBUGLN("Error setting up MDNS responder!"); }
	server_init();
	syslog(String(log_name) + F(" starting up!"));
}

void loop() {
	static String name;
	// Check for card reader data
	if (reader1_count >= CARD_LEN && (reader1_millis + 5 <= millis() || millis() < 10)) {
		fix_reader1_string();
		name = grep_auth_file();
		if (name != "") {
			if (toggle_pin(LED_SENSE)) {
				name += " enabled " + String(log_name);
			}
			else {
				name += " disabled " + String(log_name);
			}
			append_log(name);
			syslog(name);
		}
		else if (reader1_string == DoS_id) {
			digitalWrite(D0_SENSE, HIGH);
			append_log("DoS mode enabled by control card");
		}
		else {
			append_log(reader1_string);
		}
		reader1_reset();
	}

	// Check for HTTP requests
	server.handleClient();
	ArduinoOTA.handle();
	//gpio_set_level();
	// Standard delay
	delay(100);
}
