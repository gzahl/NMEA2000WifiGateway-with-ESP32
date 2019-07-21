/*  
  This code is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.
  This code is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.
  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

// Version 0.1, 21.07.2019, AK-Homberger

#include <Arduino.h>
#include <NMEA2000_CAN.h>  // This will automatically choose right CAN library and create suitable NMEA2000 object
#include <Seasmart.h>
#include <memory>
#include <N2kMessages.h>
#include <WiFi.h>
#include <OneWire.h>
#include <OneButton.h>"
#include <DallasTemperature.h>
#include "N2kDataToNMEA0183.h"
#include "List.h"

// debug log, set to 1 to enable AIS forward on USB-Serial / 2 for ADC voltage to support calibration
#define ENABLE_DEBUG_LOG 2

#define ADC_Calibration_Value 15.5 // The real value depends on the true ressitor values for the ADC input (100K / 27 K)

#define HighTempAlarm 12  
#define LowVoltageAlarm 11

// Wifi AP
const char *ssid = "MyESP32";
const char *password = "testtest"; 

int buzzerPin = 12;   // Buzzer on GPIO 12
int buttonPin = 0;    // Button on GPIO 0 to Acknowledge alarm with Buzzer
int alarmstate=false; // Alarm state (low voltage/temperature)
int acknowledge=false; // Acknowledge for alarm, Button pressed

OneButton button(buttonPin, false); // The OneButton library is used to debonce the acknowledge button

const uint16_t ServerPort=2222; // Define the port, where served sends data. Use this e.g. on OpenCPN
const char *ServerIP=""; // Define the IP, what server will use. This has to be within your local network. Leave empty for DHCP

const size_t MaxClients=10;
bool SendNMEA0183Conversion=true; // Do we send NMEA2000 -> NMEA0183 conversion
bool SendSeaSmart=false; // Do we send NMEA2000 messages in SeaSmart format

WiFiServer server(ServerPort,MaxClients);

using tWiFiClientPtr = std::shared_ptr<WiFiClient>;
LinkedList<tWiFiClientPtr> clients;

tN2kDataToNMEA0183 tN2kDataToNMEA0183(&NMEA2000, 0);

// Set the information for other bus devices, which messages we support
const unsigned long TransmitMessages[] PROGMEM={127489L, // Engine dynamic
                                                0};
const unsigned long ReceiveMessages[] PROGMEM={/*126992L,*/ // System time
                                              127250L, // Heading
                                              127258L, // Magnetic variation
                                              128259UL,// Boat speed
                                              128267UL,// Depth
                                              129025UL,// Position
                                              129026L, // COG and SOG
                                              129029L, // GNSS
                                              130306L, // Wind
                                              128275UL,// Log
                                              127245UL,// Rudder
                                              0};

// Forward declarations
void HandleNMEA2000Msg(const tN2kMsg &N2kMsg);
void SendNMEA0183Message(const tNMEA0183Msg &NMEA0183Msg);

// Data wire for teperature (Dallas DS18B20) is plugged into GPIO 13 on the ESP32
#define ONE_WIRE_BUS 13
// Setup a oneWire instance to communicate with any OneWire devices (not just Maxim/Dallas temperature ICs)
OneWire oneWire(ONE_WIRE_BUS);
// Pass our oneWire reference to Dallas Temperature. 
DallasTemperature sensors(&oneWire);


#define NavigationSendOffset 0
#define EnvironmentalSendOffset 40
#define BatterySendOffset 80
#define MiscSendOffset 120

#define SlowDataUpdatePeriod 1000  // Time between CAN Messages sent

// Battery Voltage is connected GPIO 34 (Analog ADC1_CH6) 
const int ADCpin = 34;
float voltage=0;
float temp=0;

// Task handle for OneWire read (Core 0 on ESP32)
TaskHandle_t Task1;

// serial port 2 config (GPIO 16)
const int baudrate = 38400;
const int rs_config = SERIAL_8N1;

// buffer config

#define MAX_NMEA0183_MESSAGE_SIZE 150 // For AIS
byte buff[MAX_NMEA0183_MESSAGE_SIZE];


// UPD broadcast for Navionice, OpenCPN, etc.
const char * udpAddress = "192.168.4.255"; // UDP broadcast address. Should be the network of the ESP32 AP (please check)
const int udpPort = 2000; // port 2000 lets think Navionics it is an DY WLN10 device

// create UDP instance
WiFiUDP udp;
  
void debug_log(char* str) {
#if ENABLE_DEBUG_LOG == 1
   Serial.println(str);
#endif
}
  
void setup() {
   
   pinMode(buzzerPin, OUTPUT);
   pinMode(buttonPin, INPUT_PULLUP);
   
   button.attachClick(clickedIt);

// init USB Serial port
   Serial.begin(115200);

   // init AIS serial port 2
   Serial2.begin(baudrate, rs_config);

// init wifi connection
   WiFi.mode(WIFI_AP);
   WiFi.softAP(ssid, password);

   IPAddress IP = WiFi.softAPIP();
   Serial.print("AP IP address: ");
   Serial.println(IP);
   
// Start OneWire
   sensors.begin();

// Start TCP Server
   server.begin();

  
// Reserve enough buffer for sending all messages. This does not work on small memory devices like Uno or Mega
  
   NMEA2000.SetN2kCANMsgBufSize(8);
   NMEA2000.SetN2kCANReceiveFrameBufSize(250);
   NMEA2000.SetN2kCANSendFrameBufSize(250);

   
// Set Product information
   NMEA2000.SetProductInformation("00000001", // Manufacturer's Model serial code
                                 100, // Manufacturer's product code
                                 "My Boat Name",  // Manufacturer's Model ID
                                 "1.0.2.25 (2019-07-07)",  // Manufacturer's Software version code
                                 "1.0.2.0 (2019-07-07)" // Manufacturer's Model version
                                 );
// Set device information
   NMEA2000.SetDeviceInformation(1, // Unique number. Use e.g. Serial number.
                                132, // Device function=Analog to NMEA 2000 Gateway. See codes on http://www.nmea.org/Assets/20120726%20nmea%202000%20class%20&%20function%20codes%20v%202.00.pdf
                                25, // Device class=Inter/Intranetwork Device. See codes on  http://www.nmea.org/Assets/20120726%20nmea%202000%20class%20&%20function%20codes%20v%202.00.pdf
                                2046 // Just choosen free from code list on http://www.nmea.org/Assets/20121020%20nmea%202000%20registration%20list.pdf                               
                               );
  
// If you also want to see all traffic on the bus use N2km_ListenAndNode instead of N2km_NodeOnly below

   NMEA2000.SetForwardType(tNMEA2000::fwdt_Text); // Show in clear text. Leave uncommented for default Actisense format.
   NMEA2000.SetMode(tNMEA2000::N2km_ListenAndNode,32);

   NMEA2000.ExtendTransmitMessages(TransmitMessages);
   NMEA2000.ExtendReceiveMessages(ReceiveMessages);
   NMEA2000.AttachMsgHandler(&tN2kDataToNMEA0183); // NMEA 2000 -> NMEA 0183 conversion
   NMEA2000.SetMsgHandler(HandleNMEA2000Msg); // Also send all NMEA2000 messages in SeaSmart format
  
   tN2kDataToNMEA0183.SetSendNMEA0183MessageCallback(SendNMEA0183Message);

   NMEA2000.Open();

// Create task for core 0, loop() runs on core 1
   xTaskCreatePinnedToCore(
      GetTemperature, /* Function to implement the task */
      "Task1", /* Name of the task */
      10000,  /* Stack size in words */
      NULL,  /* Task input parameter */
      2,  /* Priority of the task */
      &Task1,  /* Task handle. */
      0); /* Core where the task should run */
      
   delay(200);  
}

// This task runs isolated on core 0 because sensors.requestTemperatures() is slow and blocking for about 750 ms
void GetTemperature( void * parameter) {
  float tmp=0;
  for(;;) {
  sensors.requestTemperatures(); // Send the command to get temperatures
  tmp = sensors.getTempCByIndex(0);
  if(tmp!=-127) temp=tmp;
  delay(200);
  }
}




//*****************************************************************************
void SendBufToClients(const char *buf) {
  for (auto it=clients.begin() ;it!=clients.end(); it++) {
    if ( (*it)!=NULL && (*it)->connected() ) {
      (*it)->println(buf);
    }
  }
}

#define MAX_NMEA2000_MESSAGE_SEASMART_SIZE 500 
//*****************************************************************************
//NMEA 2000 message handler
void HandleNMEA2000Msg(const tN2kMsg &N2kMsg) {
  

  if ( !SendSeaSmart ) return;
  
  char buf[MAX_NMEA2000_MESSAGE_SEASMART_SIZE];
  if ( N2kToSeasmart(N2kMsg,millis(),buf,MAX_NMEA2000_MESSAGE_SEASMART_SIZE)==0 ) return;
  SendBufToClients(buf);
}


//*****************************************************************************
void SendNMEA0183Message(const tNMEA0183Msg &NMEA0183Msg) {
  if ( !SendNMEA0183Conversion ) return;
  
  char buf[MAX_NMEA0183_MESSAGE_SIZE];
  if ( !NMEA0183Msg.GetMessage(buf,MAX_NMEA0183_MESSAGE_SIZE) ) return;
  SendBufToClients(buf);
}


bool IsTimeToUpdate(unsigned long NextUpdate) {
  return (NextUpdate<millis());
}
unsigned long InitNextUpdate(unsigned long Period, unsigned long Offset=0) {
  return millis()+Period+Offset;
}

void SetNextUpdate(unsigned long &NextUpdate, unsigned long Period) {
  while ( NextUpdate<millis() ) NextUpdate+=Period;
}


void SendN2kEngine() {
  static unsigned long SlowDataUpdated=InitNextUpdate(SlowDataUpdatePeriod,MiscSendOffset);
  tN2kMsg N2kMsg;
  
  if ( IsTimeToUpdate(SlowDataUpdated) ) {
    SetNextUpdate(SlowDataUpdated,SlowDataUpdatePeriod);

    SetN2kEngineDynamicParam(N2kMsg,0,N2kDoubleNA,N2kDoubleNA,CToKelvin(temp),voltage,N2kDoubleNA,N2kDoubleNA,N2kDoubleNA,N2kDoubleNA,N2kInt8NA,N2kInt8NA,true);
    NMEA2000.SendMsg(N2kMsg);
  }
}


//*****************************************************************************
void AddClient(WiFiClient &client) {
  Serial.println("New Client.");
  clients.push_back(tWiFiClientPtr(new WiFiClient(client)));
}

//*****************************************************************************
void StopClient(LinkedList<tWiFiClientPtr>::iterator &it) {
  Serial.println("Client Disconnected.");
  (*it)->stop();
  it=clients.erase(it);
}

//*****************************************************************************
void CheckConnections() {
 WiFiClient client = server.available();   // listen for incoming clients

  if ( client ) AddClient(client);

  for (auto it=clients.begin(); it!=clients.end(); it++) {
    if ( (*it)!=NULL ) {
      if ( !(*it)->connected() ) {
        StopClient(it);
      } else {
        if ( (*it)->available() ) {
          char c = (*it)->read();
          if ( c==0x03 ) StopClient(it); // Close connection by ctrl-c
        }
      }
    } else {
      it=clients.erase(it); // Should have been erased by StopClient
    }
  }
}

// ReadVoltage is used to improve the linearity of the ESP32 ADC see: https://github.com/G6EJD/ESP32-ADC-Accuracy-Improvement-function

double ReadVoltage(byte pin){
  double reading = analogRead(pin); // Reference voltage is 3v3 so maximum reading is 3v3 = 4095 in range 0 to 4095
  if(reading < 1 || reading > 4095) return 0;
  // return -0.000000000009824 * pow(reading,3) + 0.000000016557283 * pow(reading,2) + 0.000854596860691 * reading + 0.065440348345433;
  return (-0.000000000000016 * pow(reading,4) + 0.000000000118171 * pow(reading,3)- 0.000000301211691 * pow(reading,2)+ 0.001109019271794 * reading + 0.034143524634089)*1000;
} // Added an improved polynomial, use either, comment out as required

  
void loop() {
unsigned int size = 0;
// read data from serial 2 and send to wifi as UDP multicast and to TCP clients

  while ((size = Serial2.available())) {
           size = (size >= MAX_NMEA0183_MESSAGE_SIZE ? MAX_NMEA0183_MESSAGE_SIZE : size);
           Serial2.readBytes(buff, size);
           
           #if ENABLE_DEBUG_LOG == 1
              Serial.write(buff, size);              
           #endif           

           udp.beginPacket(udpAddress, udpPort);
           udp.write(buff, size);
           udp.endPacket();
  
           // SendBufToClients((char*)buff); // Uncomment this for sending AIS to TCP. This implementation is not clean because it is not always ensured that the full AIS message has been received. But it is working.
   }
  
  voltage=((voltage*15)+(ReadVoltage(ADCpin)*ADC_Calibration_Value/4096)) /16; // This implements a low pass filter to eliminate spike for ADC readings
  
  SendN2kEngine();
  CheckConnections();
  NMEA2000.ParseMessages();
  tN2kDataToNMEA0183.Update();

  // Dummy to empty input buffer to avoid board to stuck with e.g. NMEA Reader
  if ( Serial.available() ) { Serial.read(); } 

// Alarm handling
  button.tick();

  #if ENABLE_DEBUG_LOG == 2
      Serial.print("Spannung:" ); Serial.println(voltage);              
      //Serial.print("Temperatur: ");Serial.println(temp);              
      Serial.println("");
  #endif           

  
  alarmstate=false;
  if (temp > HighTempAlarm || voltage < LowVoltageAlarm) {alarmstate=true;}
  if (alarmstate==true && acknowledge==false) {digitalWrite(buzzerPin, HIGH);} else {digitalWrite(buzzerPin, LOW);}

}

void clickedIt() {
  acknowledge = !acknowledge;
 }











   