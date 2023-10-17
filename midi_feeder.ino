#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>

#if defined(ESP8266)
    #pragma message "INFO: ESP8266 stuff happening!"
    #define MIDI_PAD_PIN A0
    #define MIDI_NOTE_INDICATOR_PIN LED_BUILTIN
    #define ADC_RESOLUTION_MAX_VALUE 1023
#elif defined(ESP32)
    #pragma message "INFO: ESP32 stuff happening!"
    #define MIDI_PAD_PIN 13
    #define MIDI_NOTE_INDICATOR_PIN 33
    #define ADC_RESOLUTION_MAX_VALUE 4095
#else
    #error "ERROR: This ain't a ESP8266 or ESP32, dumbo!"
#endif

#define MODE_DEBUG true

#if defined(MODE_DEBUG)
    #define SERIAL_RATE 9600 // ESP8266 NodeMCU V3 recommended 
#else
    #define SERIAL_RATE 31250 // MIDI baud rate
#endif

// Piezo/MIDI definitions
#define MIDI_THRESHOLD_MIN 30 // sygnal < MIDI_THRESHOLD_MIN is treated as no sygnal
#define MIDI_NOTE_INDICATOR_TIME 100 // How long led will indicate a note
#define MIDI_DEBUG_MESSAGE_PERIOD 1000 // How long led will indicate a note
//Program defines (time measured in milliseconds)
#define MIDI_SIGNAL_BUFFER_SIZE 100 // 30
#define MIDI_PEAK_BUFFER_SIZE 30 // 30
#define MIDI_MAX_TIME_BETWEEN_PEAKS 20
#define MIDI_MIN_TIME_BETWEEN_NOTES 50
// General MIDI drum code numbers
// See http://soundprogramming.net/file-formats/general-midi-drum-note-numbers/
#define MIDI_NOTE_SNARE_CMD 38
// #define MIDI_NOTE_HAND_CLAP_CMD 39
// #define MIDI_NOTE_SNARE_E_CMD 40
// #define MIDI_NOTE_FLOOR_TOM_CMD 43
// #define MIDI_NOTE_COWBEL_CMD 56
// #define MIDI_NOTE_BONGO_HIGH_CMD 60
// #define MIDI_NOTE_BONGO_LOW_CMD 61
// #define MIDI_NOTE_CONGA_DEAD_STROKE_CMD 62
// #define MIDI_NOTE_CONGA_CMD 63
// #define MIDI_COMMAND_CONTROL_CHANGE 0xB0
#define MIDI_COMMAND_NOTE_ON 0x90
#define MIDI_COMMAND_NOTE_OFF 0x80
#define MIDI_MAX_VELOCITY 127
#define MIDI_MIN_VELOCITY 40
// END: Piezo/MIDI definitions

// WIFI definitions
#define WIFI_CLIENT_RECONNECT_PERIOD 5000
// WIFI definitions

const char * const ssid = "WIFItoNawetNieEstOno";
const char * const password = "FleXa123";
// const char *ssid     = "WIFItoNawetNieEstOno";
// const char *password = "FleXa123";
const char * const host = "127.0.0.1";
const uint16_t port = 8080;


// MIDI variables
// Ring buffers to store analog signal and peaks
short currentSignalIndex;
short currentPeakIndex;
unsigned short signalBuffer[MIDI_SIGNAL_BUFFER_SIZE];
unsigned short peakBuffer[MIDI_PEAK_BUFFER_SIZE];
unsigned short noteReadyVelocity;
unsigned long lastPeakTime;
unsigned long lastNoteTime;
unsigned long lastDebugMessageTime;
bool isNoteReady;
bool isLastPeakZeroed;
bool isNoteIndicationOn = false; // Only one led indicator for all notes.

unsigned long lastWiFiConnectionTime;
unsigned long lastWiFiReonnectionAttemptTime;

String ip = "";

char buffer[15];

/**
 * Check is any note was triggered recently to blink with note indicator
 */
void checkNoteIndication()
{
    unsigned long currentTime = millis();

    if (true == isNoteIndicationOn) {
        digitalWrite(MIDI_NOTE_INDICATOR_PIN, HIGH);
        isNoteIndicationOn = false;
    } else if ((currentTime - lastNoteTime) > MIDI_NOTE_INDICATOR_TIME) {
        digitalWrite(MIDI_NOTE_INDICATOR_PIN, LOW);
    }
}

/**
 * Send note ON command to serial
 *
 * @param note
 * @param midiVelocity
 */
void midiNoteOn(int note, byte midiVelocity)
{
    #if defined(MODE_DEBUG)
        // Debug serial
        Serial.print("ON: ");
        Serial.print(note);
        Serial.print(":");
        Serial.print(midiVelocity);
        Serial.println();
    #else
        Serial.write(MIDI_COMMAND_NOTE_ON);
        Serial.write((byte) note);
        Serial.write((byte) midiVelocity);
    #endif
}

/**
 * Send note OFF command to serial
 *
 * @param note
 * @param midiVelocity
 */
void midiNoteOff(int note, byte midiVelocity)
{
    #if defined(MODE_DEBUG)
        // Debug serial
        Serial.print("OFF: ");
        Serial.print(note);
        Serial.println();
    #else
        Serial.write(MIDI_COMMAND_NOTE_OFF);
        Serial.write(note);
        Serial.write(0);
    #endif
}

/**
 * Fire note and raise all related flags
 *
 * @param note
 * @param velocity
 */
void noteFire(int note, unsigned short velocity)
{
    // Protection: remove MSBs on data
    note &= 0x7f;
    velocity &= 0x7f;

    midiNoteOn(note, velocity);
    midiNoteOff(note, velocity);

    lastNoteTime = millis();
    isNoteIndicationOn = true;
}

/**
 * 1 of 3 cases can happen:
 * 
 * 1) note ready - if new peak >= previous peak - signal wave is raising but not on peak.
 * 2) note fire - if new peak < previous peak and previous peak was a note ready - signal wave started to go down after raising.
 * 3) no note - if new peak < previous peak and previous peak was NOT note ready - signal wave going down not after raising.
 */
void recordNewPeak(short newPeak)
{
    isLastPeakZeroed = (0 == newPeak);

    unsigned long currentTime = millis();
    lastPeakTime = currentTime;

    // new peak recorded (newPeak)
    peakBuffer[currentPeakIndex] = newPeak;

    // get previous peak from buffer
    short prevPeakIndex = currentPeakIndex - 1;
    if (prevPeakIndex < 0) prevPeakIndex = MIDI_PEAK_BUFFER_SIZE - 1;
    unsigned short prevPeak = peakBuffer[prevPeakIndex];

    if (newPeak > prevPeak && (currentTime - lastNoteTime) > MIDI_MIN_TIME_BETWEEN_NOTES) {

        // 1) note ready - if new peak >= previous peak - signal wave is raising but not on peak.
        isNoteReady = true;
        if (newPeak > noteReadyVelocity) noteReadyVelocity = newPeak;

    } else if (newPeak < prevPeak && true == isNoteReady) {

        // 2) note fire - if new peak < previous peak and previous peak was a note ready - signal wave started to go down after raising.
        unsigned short velocity = noteReadyVelocity;

        #if defined(MODE_DEBUG)
            Serial.print("Velocity RAW: ");
            Serial.print(velocity);
            Serial.println();
        #endif
        velocity = map(velocity, MIDI_THRESHOLD_MIN, ADC_RESOLUTION_MAX_VALUE, MIDI_MIN_VELOCITY, MIDI_MAX_VELOCITY);

        noteFire(MIDI_NOTE_SNARE_CMD, velocity);

        isNoteReady = false;
        noteReadyVelocity = 0;
    }

    // 3) no note - if new peak < previous peak and previous peak was NOT note ready - signal wave going down not after raising.
    checkNoteIndication();

    #if defined(MODE_DEBUG)
        if ((currentTime - lastDebugMessageTime) > MIDI_DEBUG_MESSAGE_PERIOD) {
            Serial.print("DEBUG newPeak: ");
            Serial.print(newPeak);
            Serial.println();
            lastDebugMessageTime = currentTime;
        }
    #endif

    currentPeakIndex++;
    if (currentPeakIndex == MIDI_PEAK_BUFFER_SIZE) currentPeakIndex = 0;
}

void listenPad()
{
    unsigned short newSignal = analogRead(MIDI_PAD_PIN);
    unsigned long currentTime = millis();

    signalBuffer[currentSignalIndex] = newSignal;

    if (newSignal < MIDI_THRESHOLD_MIN) {

        if (false == isLastPeakZeroed && (currentTime - lastPeakTime) > MIDI_MAX_TIME_BETWEEN_PEAKS) {

            recordNewPeak(0);
        } else {

            short prevSignalIndex = currentSignalIndex - 1;
            if (prevSignalIndex < 0) prevSignalIndex = MIDI_SIGNAL_BUFFER_SIZE - 1;

            unsigned short prevSignal = signalBuffer[prevSignalIndex];
            unsigned short latestSignalPeak = 0;

            // find the wave peak if previous signal was not 0 
            // by going through previous signal values until another 0 is reached
            while (prevSignal >= MIDI_THRESHOLD_MIN) {
                if (signalBuffer[prevSignalIndex] > latestSignalPeak) {
                    latestSignalPeak = signalBuffer[prevSignalIndex];
                }

                // decrement previous signal index, and get previous signal
                prevSignalIndex--;

                if (prevSignalIndex < 0) prevSignalIndex = MIDI_SIGNAL_BUFFER_SIZE - 1;

                prevSignal = signalBuffer[prevSignalIndex];
            }

            if (latestSignalPeak > 0) {
                recordNewPeak(latestSignalPeak);
            }
        }
    }

    currentSignalIndex++;
    if (currentSignalIndex == MIDI_SIGNAL_BUFFER_SIZE) currentSignalIndex = 0;
}


void postHttpRequestSimple(String httpRequestData)
{
    unsigned long currentTime = millis();

    if (currentTime - lastWiFiConnectionTime < WIFI_CLIENT_RECONNECT_PERIOD) {
        return;
    }

    WiFiClient client;

    Serial.print("connecting to ");
    Serial.print(host);
    Serial.print(':');
    Serial.println(port);

    if (currentTime - lastWiFiReonnectionAttemptTime < WIFI_CLIENT_RECONNECT_PERIOD) {
        return;
    }

    // Use WiFiClient class to create TCP connections
    if (!client.connect(host, port)) {
        Serial.println("connection failedstill. waiting.");
        lastWiFiReonnectionAttemptTime = currentTime;
        // delay(5000);
        return;
    }

    // This will send a string to the server
    Serial.println("sending data to server");
    if (client.connected()) { 
        client.println(httpRequestData);
    }

    // wait for data to be available
    unsigned long timeout = millis();
    while (client.available() == 0) {
        if (currentTime - timeout > 5000) {
            Serial.println(">>> Client Timeout !");
            client.stop();
            // delay(60000);
            return;
        }
    }

    // Read all the lines of the reply from server and print them to Serial
    Serial.println("receiving from remote server");
    // not testing 'client.connected()' since we do not need to send data here
    while (client.available()) {
        char ch = static_cast<char>(client.read());
        Serial.print(ch);
    }

    // Close the connection
    Serial.println();
    Serial.println("closing connection");
    client.stop();

    lastWiFiConnectionTime = currentTime;
}

void postHttpRequest(String httpRequestData)
{
    unsigned long currentTime = millis();

    if (currentTime - lastWiFiConnectionTime < WIFI_CLIENT_RECONNECT_PERIOD) {
        return;
    }

    if (WiFi.status() == WL_CONNECTED) {
        WiFiClient client;
        HTTPClient http;

        http.begin(client, host);

        // If you need Node-RED/server authentication, insert user and password below
        //http.setAuthorization("REPLACE_WITH_SERVER_USERNAME", "REPLACE_WITH_SERVER_PASSWORD");

        // http.addHeader("Content-Type", "application/x-www-form-urlencoded");
        // String httpRequestData = "api_key=tPmAT5Ab3j7F9&sensor=BME280&value1=24.25&value2=49.54&value3=1005.14";           
        // int httpResponseCode = http.POST(httpRequestData);

        // If you need an HTTP request with a content type: application/json, use the following:
        // http.addHeader("Content-Type", "application/json");
        // int httpResponseCode = http.POST("{\"api_key\":\"tPmAT5Ab3j7F9\",\"sensor\":\"BME280\",\"value1\":\"24.25\",\"value2\":\"49.54\",\"value3\":\"1005.14\"}");

        // If you need an HTTP request with a content type: text/plain
        http.addHeader("Content-Type", "text/plain");
        int httpResponseCode = http.POST(httpRequestData);

        Serial.print("HTTP Response code: ");
        Serial.println(httpResponseCode);

        // Free resources
        http.end();
    }
    else {
        Serial.println("WiFi Disconnected");
    }

    lastWiFiConnectionTime = currentTime;
}

void initMidi()
{
    currentSignalIndex = 0;
    currentPeakIndex = 0;
    memset(signalBuffer, 0, sizeof(signalBuffer));
    memset(peakBuffer, 0, sizeof(peakBuffer));
    isNoteReady = false;
    noteReadyVelocity = 0;
    isLastPeakZeroed = true;
    isNoteIndicationOn = false;
    lastPeakTime = 0;
    lastNoteTime = 0;
    lastDebugMessageTime = 0;
}

void initWiFi()
{
    lastWiFiConnectionTime = 0;
    lastWiFiReonnectionAttemptTime = 0;

    #ifdef MODE_DEBUG
        Serial.setDebugOutput(true);
    #endif

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);

    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }

    ip = WiFi.localIP().toString();

    Serial.println("WiFi connected");
    Serial.println("IP address: ");
    Serial.println(ip);
    
    // ESP8266 has the ability to automatically reconnect to your router
    // WiFi.setAutoReconnect(true);
    // WiFi.persistent(true);
}

void setup() {
    pinMode(MIDI_NOTE_INDICATOR_PIN, OUTPUT);
    pinMode(MIDI_PAD_PIN, OUTPUT);
    digitalWrite(MIDI_PAD_PIN, LOW);

    Serial.begin(SERIAL_RATE);
    
    delay(10);

    initWiFi();
    initMidi();
}

void loop()
{
    listenPad();

    ltoa(lastNoteTime, buffer, 16);
    postHttpRequestSimple(buffer);
}
