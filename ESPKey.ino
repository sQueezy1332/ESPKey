#include "espkey.h"

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
	pinMode(CONF_RESET, INPUT_PULLUP);
	pinMode(2, OUTPUT_OPEN_DRAIN); delay(5000);
	// Input interrupts
	//attachInterrupt(digitalPinToInterrupt(D0_SENSE), reader1_D0_trigger, FALLING);
	//attachInterrupt(digitalPinToInterrupt(D1_SENSE), reader1_D1_trigger, FALLING);
	//attachInterrupt(digitalPinToInterrupt(LED_SENSE), auxChange, CHANGE);
	//attachInterrupt(digitalPinToInterrupt(CONF_RESET), resetConfig, CHANGE);
#ifdef DEBUG_ENABLE
	Serial.begin(115200);
	delay(100);
	//Serial.setDebugOutput(true);
	led_blink();
	DEBUGLN("Chip ID: 0x" + String(ESP.getChipId(), HEX)); delay(2000);
	digitalWrite(2, 0); 
	while (true) {}

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
	// Check for card reader data
	if (reader_count && (millis() > reader1_millis + 5 /*|| millis() < 10*/)) {
		//fix_reader_string();
		reader_string = String(reader_code, HEX); reader_string += ':' + String(reader_count);
		String name(grep_auth_file());
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
		//else if (reader_string == DoS_id) {
		//	digitalWrite(D0_SENSE, HIGH);
		//	append_log("DoS mode enabled by control card");
		//}
		else append_log(reader_string);
		reader_reset();
	}

	// Check for HTTP requests
	server.handleClient();
	ArduinoOTA.handle();
	//gpio_set_level();
	// Standard delay
	delay(100);
}
