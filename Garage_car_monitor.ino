/*
   Sketch to learn how to set up web config on ESP8266
*/
#include <ArduinoJson.h>

#include <ESP8266WiFi.h>
#include <EEPROM.h>
#include <ESP8266WebServer.h>
#include <WiFiClient.h>
#include <PubSubClient.h>
/*
   Changes to PubSubClient library
   The status update is bigger than 128 bytes so it is necessary to change MQTT_MAX_PACKET_SIZE to 256 in PubSubClient.h
*/

/*
   Script specific
*/
#define SOFTWARE  "Garage_car_monitor"
#define SW_VERSION 0.1

/*
   General declarations
*/
#define ADMIN_ON  300000    // 5 minutes should be enough
#define NETWORKCHECK 15000    // check the network every 15 sec

#define ADMIN_PIN D8
#define ECHO      D1
#define TRIGGER   D4
#define CHECK_INTERVAL  2000    // the delay interval in microseconds

/*
   General variables
*/
long int adminTimer = 0;
long int sensorTimer = 0;
bool adminMode = false;
int wifiStatus = -1;
long int connectionTimer = 0;
long int status_timer = 0;
long int network_timer = 0;
boolean stringComplete = false;  // whether the string is complete
String inputString = "";         // a string to hold incoming data
String sJSON = "";

int toggle = 0;
long duration;
float distance;
float aveDist;
long int dataUpdate = 0;
long int statusUpdate = 0;

// This array stores the last 5 readings and enable us to take averages
float lastDist[] = {0.0, 0.0, 0.0, 0.0, 0.0};

// create web client and mqtt client
WiFiClient wifiClient;
PubSubClient mqtt_client(wifiClient);

// create the web server object
ESP8266WebServer server(80);

#include "led_colours.h"

#include "config.h"

#include "web_helpers.h"
#include "mqtt_helpers.h"
#include "led_helpers.h"
#include "web_pages.h"

// this is used to check the input voltage
ADC_MODE(ADC_VCC);

/*
   All set up code here
*/
void setup() {
  // put your setup code here, to run once:
  EEPROM.begin(512);
  Serial.begin(115200);
  Serial.println("Starting ESP8266");

  pinMode(ADMIN_PIN, INPUT_PULLUP);

  // Read the config
  ReadConfig();
  configPrint();

  // set wifi to client mode, join wifi & print mac address
  WiFi.mode(WIFI_STA);
  WiFi.begin(config.ssid.c_str(), config.password.c_str());
  Serial.print("MAC Address is ");
  Serial.println(WiFi.macAddress());

  // set up the LED environment
  pixels.begin();
  pixels.setBrightness(64);
  pixels.setPixelColor(MAINLED, LED_red);
  pixels.show();
  delay(500);
  pixels.setPixelColor(MAINLED, LED_green);
  pixels.show();
  delay(500);
  pixels.setPixelColor(MAINLED, LED_red);
  pixels.show();
  delay(500);
  pixels.setPixelColor(MAINLED, LED_blank);
  pixels.show();

  //setPrimaryLED(LED_green);

  // set up the ultrasonic sensor
  pinMode(TRIGGER, OUTPUT); // Sets the trigPin as an Output
  pinMode(ECHO, INPUT); // Sets the echoPin as an Input


}

/*
   This is the main loop of the sketch
*/
void loop() {
  // put your main code here, to run repeatedly:

  serialEvent();            // call the function

  // if the admin button is pressed, go into admin mode
  if (digitalRead(ADMIN_PIN))
  {
    startAdmin();
  }

  //Serial.println(digitalRead(ADMIN_PIN));

  // this code controls the LED
  showLED();

  // at this time admin mode is triggered from entering 'a' at the serial console
  if (adminMode)
  {
    // the web server, if running, handles web traffic
    server.handleClient();

    if ((adminTimer + ADMIN_ON) < millis())   // run out of time, turn the admin function off
    {
      stopAdmin();// defined in config.h
    }
  }

  // check the network on a regular basis
  if ((network_timer + NETWORKCHECK) > millis())
  {
    network_timer = millis();
    showWifiStatus();   // shows wifi status on serial console
  }

  // send a regular status update from the node
  if ((status_timer + (config.statusPeriod * 1000) < millis()) || (millis() < status_timer))
  {

    mqtt_send_status();

    status_timer = millis();
  }

  if (((sensorTimer + CHECK_INTERVAL) < millis()) || (millis() < sensorTimer))
  {
    sensorTimer = millis();

    // Sets the trigPin on HIGH state for 10 micro seconds
    digitalWrite(TRIGGER, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIGGER, LOW);

    // Reads the echoPin, returns the sound wave travel time in microseconds
    duration = pulseIn(ECHO, HIGH);

    // Calculating the distance
    distance = duration * 0.034 / 2;
    // Prints the distance on the Serial Monitor
    //Serial.print("Distance: ");
    //Serial.println(distance);
    aveDist = ave_dist(distance);
    //Serial.print("Average dist = ");
    //Serial.println(aveDist);
    

    // if the measured distance  is more than 5 cm diffent from an average over 5 readings then send the data out
    // or send it out if greater than config.dataPeriod seconds has elapsed
    if (((distance - aveDist) > 5.0) || ((distance - aveDist) < -5.0) || ((millis() - (config.dataPeriod * 1000)) > dataUpdate))
    {

    // define a JSON structure for the payload
      StaticJsonBuffer<200> jsonBuffer;
      JsonObject& jPayload = jsonBuffer.createObject();

      // We send a message on a 'Regular' basis. Other messages are because a variance is detected
      if ((millis() - (config.dataPeriod * 1000)) > dataUpdate)
        jPayload["Type"] = "Regular";
      else
        jPayload["Type"] = "Variance";

      jPayload["Distance"] = distance;
      Serial.println(config.dataPeriod);
      Serial.println(dataUpdate);
      
      jPayload.printTo(Serial);
      Serial.println();
      String js;
      jPayload.printTo(js);
      sendMQTT(config.MQTT_Topic1, js);

      dataUpdate = millis();        //reset the timer
    }
  }
}


/*
   Function to handle serial input
*/
void serialEvent() {
  while (Serial.available()) {
    // get the new byte:
    char inChar = (char)Serial.read();
    // add it to the inputString:
    inputString += inChar;
    Serial.println(inputString);
    // if the incoming character is a newline, set a flag
    // so the main loop can do something about it:
    if (inChar == 'a')
    {
      startAdmin();   // defined in config.h
    }
  }
}

//*********************************************************************************************
// Defines average distance
//*********************************************************************************************
float ave_dist(float dist)
{
  float totDist = dist;
  totDist += lastDist[3];
  lastDist[4] = lastDist[3];
  totDist += lastDist[2];
  lastDist[3] = lastDist[2];
  totDist += lastDist[1];
  lastDist[2] = lastDist[1];
  totDist += lastDist[0];
  lastDist[1] = lastDist[0];
  lastDist[0] = dist;
  return (totDist / 5);
}


