#include "espkey.h"

void setup() {
	pinMode(PIN_LED, OUTPUT);//digitalWrite(PIN_LED, HIGH);
#ifdef DEBUG_ENABLE
	Serial.begin(115200);
	delay(100);
	Serial.setDebugOutput(true);
	DEBUGLN("Chip ID: 0x" + String(ESP.getChipId(), HEX));
	//while (true) { yield(); }
#endif // DEBUG_ENABLE
	led_blink();
	pinMode(PIN_D0, OUTPUT_OPEN_DRAIN); digitalWrite(PIN_D0, HIGH);
	pinMode(PIN_D1, OUTPUT_OPEN_DRAIN); digitalWrite(PIN_D1, HIGH);
	pinMode(PIN_MODE, INPUT_PULLUP);
	pinMode(PIN_CONF_RESET, INPUT_PULLUP);
	if (dRead(PIN_MODE)) {
		attachInterrupt(digitalPinToInterrupt(PIN_D0), reader1_D0_trigger, FALLING);
		attachInterrupt(digitalPinToInterrupt(PIN_D1), reader1_D1_trigger, FALLING);
	} else {
		onewire_mode = true;
		attachInterrupt(digitalPinToInterrupt(PIN_D0), onewire_presence, FALLING);
	}
	pinMode(PIN_MODE, INPUT);
	attachInterrupt(digitalPinToInterrupt(PIN_CONF_RESET), resetConfig, CHANGE);
	//attachInterrupt(digitalPinToInterrupt(PIN_LED), auxChange, CHANGE);
	// Set Hostname.
	//String dhcp_hostname(HOSTNAME);
	//dhcp_hostname += String(ESP.getChipId(), HEX);
	WiFi.hostname(HOSTNAME);
	//DEBUGLN("Hostname: " + dhcp_hostname);

	if (!SPIFFS.begin()) {
		DEBUGLN(F("Failed to mount file system"));
		return;
	} else {
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
	append_log(F("Starting up!"));

	// If a log.txt exists, use ap_ssid=ESPKey-<chipid> instead of the default ESPKey-config
	// A config file will take precedence over this
	//if (SPIFFS.exists("/log.txt")) dhcp_hostname.toCharArray(ap_ssid, sizeof(ap_ssid));

	// Load config file.
	if (!loadConfig()) {
		DEBUGLN(F("No configuration information available.  Using defaults."));
	}
	if (!wifi_sta_init())
		if (ap_enable) {
			DEBUGLN(F("Can not connect to WiFi station. Going into AP mode."));
			// Go into software AP mode.
			WiFi.mode(WIFI_AP);
			WiFi.softAPConfig(ap_ip, ap_ip, IPAddress(255, 255, 255, 0));
			WiFi.softAP(ap_ssid, ap_psk, 0, ap_hidden);

			DEBUG(F("IP address: "));
			DEBUGLN(WiFi.softAPIP());
		} else {
			DEBUGLN(F("Can not connect to WiFi station. Bummer."));
			WiFi.mode(WIFI_OFF);
		}
// Start OTA server.
	ArduinoOTA.setHostname(/*dhcp_hostname.c_str()*/ap_ssid.c_str());
	ArduinoOTA.setPassword(ota_password.c_str());
	ArduinoOTA.begin();
	if (MDNS.begin(mDNShost)) {
		DEBUGLN("Open http://" + String(mDNShost) + ".local/");
	} else { DEBUGLN("Error setting up MDNS responder!"); }
	server_init();
	syslog(log_name + F(" starting up!"));
}

void loop() {
	// Check for card reader data
	if (onewire_mode && presence_flag) {
		if (!onewire_handle()) {
			reader_reset();
			return;
		};
	}
	if (reader_count && (uS - reader_last > 5000 /*|| millis() < 10*/)) {
		//fix_reader_string();
		noInterrupts();
		reader_string = String(reader_code, HEX); reader_string += ':' + reader_string += reader_count;
		/*String name(grep_auth_file());
		if (name != "") {
			if (toggle_pin(PIN_LED)) {
				name += " enabled " + String(log_name);
			} else {
				name += " disabled " + String(log_name);
			}
			append_log(name);
			syslog(name);
		}
		//else if (reader_string == DoS_id) {
		//	digitalWrite(PIN_D0, HIGH);
		//	append_log("DoS mode enabled by control card");
		//}
		else */
		if(!onewire_mode){
			reader_string += '_';
			reader_string += reader_delta / (reader_count - 1);
		}
		interrupts();
		append_log(reader_string);
		reader_reset();
		return;
	}
	// Check for HTTP requests
//#ifdef ESP8266
	server.handleClient();
//#endif
	ArduinoOTA.handle();
	//gpio_set_level();
	// Standard delay
	//delay(100);
}
