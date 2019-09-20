#include <Arduino.h>
#include <FS.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>

#include <ESPEasyCfg.h>

#include <PubSubClient.h>
#include <ArduinoJson.h>

//Web server and captive portal
AsyncWebServer server(80);
ESPEasyCfg captivePortal(&server, "Cat Flap");

//Custom application parameters
ESPEasyCfgParameterGroup mqttParamGrp("MQTT");
ESPEasyCfgParameter<String> mqttServer("mqttServer", "MQTT server", "server.local");
ESPEasyCfgParameter<String> mqttUser("mqttUser", "MQTT username", "homeassistant");
ESPEasyCfgParameter<String> mqttPass("mqttPass", "MQTT password", "");
ESPEasyCfgParameter<int> mqttPort("mqttPort", "MQTT port", 1883);
ESPEasyCfgParameter<String> mqttName("mqttName", "MQTT name", "CatFlap");

WiFiClient espClient;                               // TCP client
PubSubClient client(espClient);                     // MQTT object
const unsigned long postingInterval = 10L * 1000L;  // Delay between updates, in milliseconds
static unsigned long lastPostTime = 0;              // Last time you sent to the server, in milliseconds
static char flapStatus[128];                        // Status MQTT service name
static char flapConfig[128];                        // Configuration MQTT service name
static char flapCommand[128];                       // Command MQTT service
static char flapEvent[128];                         // Event MQTT service

const char *FLAP_MODE[] = { "NORMAL", "VET", "CLOSED", "NIGHT", "LEARN", "CLEAR", "OPEN" };
const int FLAP_MODE_COUNT = 7;

HardwareSerial FlapSerial(2);

/**
 * Call back on parameter change
 */
void newState(ESPEasyCfgState state) {
  if(state == ESPEasyCfgState::Reconfigured){
    client.disconnect();
  }else if(state == ESPEasyCfgState::Connected){
    Serial.print("DNS: ");
    Serial.println(WiFi.dnsIP());
  }
}

/**
 * Callback of MQTT
 */
void callback(char* topic, byte* payload, unsigned int length) {
  String data;
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  for (int i = 0; i < length; i++) {
    data += (char)payload[i];
  }
  Serial.println(data);
  if(strcmp(topic, flapCommand) == 0){
    StaticJsonDocument<300> json;
    DeserializationError error = deserializeJson(json, data);
    if (error) {
      Serial.println("Bad JSON payload");
    }else{
      const char* type = json["type"];
      if(!type){
        Serial.println("Expecting comand type");
        return;
      }
      if(strcmp(type, "mode") == 0){
        //Changing mode
        for(int i=0;i<FLAP_MODE_COUNT;++i){
          if(strcmp(json["mode"], FLAP_MODE[i]) == 0){
            Serial.print("Changing mode to ");
            Serial.println(FLAP_MODE[i]);
            FlapSerial.write('M');
            FlapSerial.write((byte)i);
            break;
          }
        }
      }else if(strcmp(type, "config") == 0){
        const char* write = json["write"];
        if(json.containsKey("index")){
          int index = json["index"];          
          if(write && json.containsKey("value")){
            int value = json["value"];
            Serial.print("Changing setting #");
            Serial.print(index);
            Serial.print(" to ");
            Serial.println(value, DEC);            
            FlapSerial.write('C');
            FlapSerial.write('S');
            FlapSerial.write((byte)index);
            FlapSerial.write((byte)(value & 0xFF));
            FlapSerial.write((byte)((value>>8) & 0xFF));            
          }else if(!write){
            Serial.print("Reading setting #");
            Serial.println(index);
            FlapSerial.write('C');
            FlapSerial.write('R');
            FlapSerial.write((byte)index);
          }else{
            Serial.println("Value to write is missing");
          }
        }
      }else{
        Serial.print("Invalid command type ");
        const char* type = json["type"];
        Serial.println(type);
      }
    }
  }
}

void setup() {
  pinMode(BUILTIN_LED, OUTPUT);
  //Serial line for debug
  Serial.begin(115200);
  //Serial line for communicating with flap
  FlapSerial.begin(38400, SERIAL_8N1, 16, 17);
  captivePortal.setLedPin(BUILTIN_LED);
  //Register custom parameters
  mqttPass.setInputType("password");
  mqttParamGrp.add(&mqttServer);
  mqttParamGrp.add(&mqttUser);
  mqttParamGrp.add(&mqttPass);
  mqttParamGrp.add(&mqttPort);
  mqttParamGrp.add(&mqttName);
  captivePortal.addParameterGroup(&mqttParamGrp);
  captivePortal.setStateHandler(newState);
  captivePortal.begin();
  server.begin();

  //Build MQTT service name
  snprintf(flapStatus, 128, "%s/Status", mqttName.getValue().c_str());
  snprintf(flapConfig, 128, "%s/Config", mqttName.getValue().c_str());
  snprintf(flapCommand, 128, "%s/Command", mqttName.getValue().c_str()); 
  snprintf(flapEvent, 128, "%s/Event", mqttName.getValue().c_str()); 

  //Setup MQTT client callbacks and port
  client.setServer(mqttServer.getValue().c_str(), mqttPort.getValue());
  client.setCallback(callback);
}

/**
 * Print a cat ID (38 bits, 12 digits) to the buffer
 * @param id 38 bits animal ID
 * @dest A 13 byte array (including trailing 0)
 */
void printCatID(uint64_t id, char* dest) {
  dest[12] = 0;
  for(int i=0;i<12;++i)
  {
    if(id>0){
      dest[11-i] = '0' + ( id % 10);
      id/= 10;
    }else{
      dest[11-i] = '0';
    }
  }
}

/**
 * Handle serial read from flap
 */
void readSerial(){
  if (FlapSerial.available()) {
    char buffer[16];
    int inByte = FlapSerial.read();
    switch(inByte){
      case 'A':
        //Acknowledge of a command
        if(FlapSerial.readBytesUntil('\n', buffer, 16)>0){
          //Status reply (always starts with M)
          if(buffer[0] == 'M'){
            StaticJsonDocument<210> root;
            root["mode"] = FLAP_MODE[(int)buffer[1]];
            uint16_t light = buffer[3];
            light += buffer[4]<<8;
            root["light"] = light;
            //Buffer 5 is P (position)
            uint16_t potPos = buffer[6];
            potPos += buffer[7]<<8;
            root["position"] = potPos;
            //Buffer 8 is S (status)
            uint16_t status = buffer[9];
            status += buffer[10]<<8;            
            root["in_lock"] = (status & 0x1) ? 1 : 0;
            root["out_lock"] = (status & 0x2) ? 1 : 0;
            root["flap_in"] = (status & 0x4) ? 1 : 0;
            root["flap_out"] = (status & 0x8) ? 1 : 0;
            //Publish to MQTT clients
            if(client.connected()){
              String msg;
              serializeJson(root, msg);
              client.publish(flapStatus, msg.c_str());
            }
          }else if(buffer[0] == 'C'){
            //Configuration received
            StaticJsonDocument<100> root;
            root["index"] = (int)buffer[1];
            //buffer[2] contains 'V' char
            uint16_t value = (buffer[3] & 0xFF);
            value |= ((buffer[4]<<8) & 0xFF00);
            root["value"] = value;
            //Publish to MQTT clients
            if(client.connected()){
              String msg;
              serializeJson(root, msg);
              client.publish(flapConfig, msg.c_str());
            }
          }else if(buffer[0] == 'E'){
            Serial.println("Error");
          }
        }
        break;

      case 'E':
        //Event (cat detected)
        if(FlapSerial.readBytesUntil('\n', buffer, 7)>0){
          char catID[13];
          uint64_t id =0;
          for(int i=0;i<6;++i){
            id += (((uint64_t)buffer[i]) << (i*8));
          }
          int ccode = (id>>38) & 0x3FF;
          id &= 0x3FFFFFFFFFLL;          
          printCatID(id, catID);
          StaticJsonDocument<100> root;
          root["country"] = ccode;
          root["id"] = catID;
          //Publish to MQTT clients
          if(client.connected()){
            String msg;
            serializeJson(root, msg);
            client.publish(flapEvent, msg.c_str());
          }
        }
    }
  }
}

void reconnect() {
  // Loop until we're reconnected
  while (!client.connected()) {
    IPAddress mqttServerIP;
    WiFi.hostByName(mqttServer.getValue().c_str(), mqttServerIP);
    Serial.print("Attempting MQTT connection to ");
    Serial.print(mqttServer.getValue().c_str());
    Serial.print(':');
    Serial.print(mqttPort.getValue());
    Serial.print('(');
    Serial.print(mqttServerIP);
    Serial.print(")...");
    // Create a Client ID baased on MAC address
    byte mac[6];                     // the MAC address of your Wifi shield
    WiFi.macAddress(mac);
    String clientId = "CatFlap-";
    clientId += String(mac[3], HEX);
    clientId += String(mac[4], HEX);
    clientId += String(mac[5], HEX);
    // Attempt to connect
    client.setServer(mqttServerIP, mqttPort.getValue());
    if (client.connect(clientId.c_str(), mqttUser.getValue().c_str(), mqttPass.getValue().c_str())) {
      Serial.println("connected");
      // Once connected, publish an announcement...
      //publishState();
      // ... and resubscribe
      client.subscribe(flapCommand);
      break;
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      client.disconnect();
      // Wait 5 seconds before retrying
      for(int i=0;i<5000;++i){
        delay(1);
        client.loop();
      }
    }
  }
}

void loop() {
  if (!client.loop() && !client.connected()) {
      reconnect();
  }else{
      readSerial();      
      unsigned long elapsed = 0;
      // if ten seconds have passed since your last connection,
      // then connect again and send data:
      elapsed = millis() - lastPostTime;  
      if (elapsed > postingInterval) {
        //publishState();
        Serial.println("Requesting status...");
        FlapSerial.print("S");
        FlapSerial.flush();
        // note the time that the update was made:
        lastPostTime = millis();
      }
  }
}