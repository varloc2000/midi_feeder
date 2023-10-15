#if defined(ESP8266)
#pragma message "INFO: ESP8266 stuff happening!"
#elif defined(ESP32)
#pragma message "INFO: ESP32 stuff happening!"
#else
#error "ERROR: This ain't a ESP8266 or ESP32, dumbo!"
#endif

#define MODE_DEBUG true
#define SERIAL_RATE 9600

// Piezo/MIDI definitions
#define MIDI_PAD_PIN 13
#define MIDI_NOTE_INDICATOR_PIN 33
#define MIDI_THRESHOLD_MIN 30 // sygnal < MIDI_THRESHOLD_MIN is treated as no sygnal
#define MIDI_NUM_THRESHOLD_LEVELS 7
#define MIDI_NOTE_INDICATOR_TIME 100
#define MIDI_NOTE_DISPLAY_TIME 300
//Program defines (time measured in milliseconds)
#define MIDI_SIGNAL_BUFFER_SIZE 100//30
#define MIDI_PEAK_BUFFER_SIZE 30//30
#define MIDI_MAX_TIME_BETWEEN_PEAKS 20
#define MIDI_MIN_TIME_BETWEEN_NOTES 50
// General MIDI drum code numbers
// See http://soundprogramming.net/file-formats/general-midi-drum-note-numbers/
#define MIDI_NOTE_SNARE_CMD 38
#define MIDI_NOTE_HAND_CLAP_CMD 39
#define MIDI_NOTE_SNARE_E_CMD 40
#define MIDI_NOTE_FLOOR_TOM_CMD 43
#define MIDI_NOTE_COWBEL_CMD 56
#define MIDI_NOTE_BONGO_HIGH_CMD 60
#define MIDI_NOTE_BONGO_LOW_CMD 61
#define MIDI_NOTE_CONGA_DEAD_STROKE_CMD 62
#define MIDI_NOTE_CONGA_CMD 63
#define MIDI_COMMAND_CONTROL_CHANGE 0xB0
#define MIDI_COMMAND_NOTE_ON 0x90
#define MIDI_COMMAND_NOTE_OFF 0x80
#define MIDI_MAX_VELOCITY 127
// END: Piezo/MIDI definitions

// MIDI variables
// Ring buffers to store analog signal and peaks
short currentSignalIndex;
short currentPeakIndex;
unsigned short signalBuffer[MIDI_SIGNAL_BUFFER_SIZE];
unsigned short peakBuffer[MIDI_PEAK_BUFFER_SIZE];
unsigned short noteReadyVelocity;
unsigned long lastPeakTime;
unsigned long lastNoteTime;
bool isNoteReady;
bool isLastPeakZeroed;
bool isNoteIndicationOn = false; // Only one led indicator for all notes.


/**
 * Check is any note was triggered recently to blink with note indicator
 */
void checkNoteIndication()
{
    unsigned long currentTime = millis();

    if (isNoteIndicationOn) {
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
    isLastPeakZeroed = (0 === newPeak);

    unsigned long currentTime = millis();
    lastPeakTime = currentTime;

    // new peak recorded (newPeak)
    peakBuffer[currentPeakIndex] = newPeak;

    //get previous peak from buffer
    short prevPeakIndex = currentPeakIndex - 1;
    if (prevPeakIndex < 0) prevPeakIndex = MIDI_PEAK_BUFFER_SIZE - 1;
    unsigned short prevPeak = peakBuffer[prevPeakIndex];

    if (newPeak > prevPeak && (currentTime - lastNoteTime) > MIDI_MIN_TIME_BETWEEN_NOTES) {
    	// 1) note ready - if new peak >= previous peak - signal wave is raising but not on peak.
        noteReady = true;
        if (newPeak > noteReadyVelocity) noteReadyVelocity = newPeak;
    } else if (newPeak < prevPeak && true === noteReady) {
        // 2) note fire - if new peak < previous peak and previous peak was a note ready - signal wave started to go down after raising.
        unsigned short velocity = noteReadyVelocity;

        // TODO: map velocity from raw analog input to 0-127 range of midi velocity before cheking max.
        // velocity = map(velocity, 0, 1023, 0, MIDI_MAX_VELOCITY);
        if (velocity > MIDI_MAX_VELOCITY) velocity = MIDI_MAX_VELOCITY;

        noteFire(MIDI_NOTE_SNARE_CMD, velocity);

        noteReady = false;
        noteReadyVelocity = 0;
        lastNoteTime = currentTime;
    }

    // 3) no note - if new peak < previous peak and previous peak was NOT note ready - signal wave going down not after raising.
    checkNoteIndication();

    currentPeakIndex++;
    if (currentPeakIndex == MIDI_PEAK_BUFFER_SIZE) currentPeakIndex = 0;
}

void listenPad()
{
    unsigned short newSignal = analogRead(MIDI_PAD_PIN);
    unsigned long currentTime = millis();

    signalBuffer[currentSignalIndex] = newSignal;

    if (newSignal < MIDI_THRESHOLD_MIN) {
        if (false === isLastPeakZeroed && (currentTime - lastPeakTime) > MIDI_MAX_TIME_BETWEEN_PEAKS) {
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
    if (currentSignalIndex == SIGNAL_BUFFER_SIZE) currentSignalIndex = 0;
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
}

void setup() {
    Serial.begin(SERIAL_RATE);

    pinMode(MIDI_PAD_PIN, OUTPUT);
    digitalWrite(MIDI_PAD_PIN, LOW);

    initMidi();
}

void loop()
{
    listenPad();
}
