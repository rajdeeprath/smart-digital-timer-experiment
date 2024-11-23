#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <Wire.h> // must be included here so that Arduino library object file references work
#include <RtcDS3231.h>
#include <ArduinoJson.h>
#include <ESP_EEPROM.h>
#include <ArduinoLog.h>
#include <stdlib.h>
#include <stdio.h>
#include <TimeLib.h>


#define countof(a) (sizeof(a) / sizeof(a[0]))
typedef int (*compfn)(const void*, const void*);
char *my_strcpy(char *destination, char *source);

RtcDS3231<TwoWire> Rtc(Wire);
IPAddress local_ip(192,168,5,1);
IPAddress gateway(192,168,5,1);
IPAddress subnet(255,255,255,0);

const int RELAY1=4;
const int RELAY2=5;
const int MAX_SCHEDULES = 20;
const int EEPROM_MAX_LIMIT = 3072;
const int EEPROM_START_ADDR = 0;
const int SCHEDULES_START_ADDR = 50;

int counter = 0;

char clientid[25];
char *ssid = "";
char *password = "12345678";

int eeAddress = 0;
int schedules_index;
boolean schedules_updated = false;
boolean relays_dirty = false;
boolean manual_mode = 0;
long lastEval = 0;

// Define a web server at port 80 for HTTP
ESP8266WebServer server(80);
RtcDateTime dtnow;

struct Settings {
   const int r1_id = 1;
   int r1_state;
   long r1_updated;
   const int r2_id = 2;
   int r2_state;
   long r2_updated;
};

struct Schedule {
   int strlength;
   char str[1024];
};

struct ScheduleItem {
   int parent_index;
   int index;
   int hh;
   int mm;
   int dow;
   char target[3];
   int target_state;
   long reg_timestamp;
   int status = 0;
};

Schedule dat = {};
Settings conf = {};
ScheduleItem user_schedules[MAX_SCHEDULES] = {};

/* Setup */
void setup() 
{
  Serial.begin(57600);
  
  while(!Serial && !Serial.available()){}
  Log.begin(LOG_LEVEL_VERBOSE, &Serial, true);

  setupEeprom();
  setupClientId();
  
  delay(5000);  
  Log.notice("Starting"CR); 
   
  setupRTC();
  //setupAP();
  setupSta();
  setupWebServer();  
}



void loop() 
{
  if(relays_dirty)
  {
    writeRelays();
    relays_dirty = false;
  }
  
  if(schedules_updated)
  {
    readSchedules();
    collectSchedule();
    sortSchedule();
    schedules_updated = false;
  }

  evaluate();
  delay(5);
  server.handleClient();
}



void setupEeprom()
{
  // start eeprom
  EEPROM.begin(EEPROM_MAX_LIMIT);
  eeAddress = 0;
  
  // Check if the EEPROM contains valid data from another run
  // If so, overwrite the 'default' values set up in our struct
  if(EEPROM.percentUsed()>=0) {
    readRelays();
    readSchedules(); 
  }
}



/**
 * Generate unique client ID
 */
void setupClientId()
{
  uint32_t chipid=ESP.getChipId();
  snprintf(clientid,25,"SB-%08X",chipid);
  ssid = clientid;
}

/**
 * Initialize access point
 */
void setupAP()
{
  //set-up the custom IP address
  WiFi.mode(WIFI_AP); 
  
  /* You can remove the password parameter if you want the AP to be open. */
  WiFi.softAP(ssid, password);
  WiFi.softAPConfig(local_ip, gateway, subnet); 
  IPAddress myIP = WiFi.softAPIP(); //Get IP address
  Log.notice("HotSpot IP: %d.%d.%d.%d"CR, myIP[0], myIP[1], myIP[2], myIP[3]);
}


/**
 * Station mode : Connects to router for testings apis
 */
void setupSta()
{
  // Connect to Wi-Fi network with SSID and password
  Log.notice("Connecting to %s"CR, ssid);
  WiFi.begin("Tomato24", "bhagawadgita@123");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Log.notice(".."CR);
  }
  // Print local IP address and start web server
  IPAddress myIP = WiFi.localIP();
  Log.notice("WiFi connected! IP address: %d.%d.%d.%d"CR, myIP[0], myIP[1], myIP[2], myIP[3]);
}

/**
 * Setup webserver
 */
void setupWebServer()
{
  server.on ( "/", handleRoot );
  server.on ( "/alarms/get", HTTP_GET, getSchedules );
  server.on ( "/alarms/set", HTTP_POST, setSchedules );
  server.on ( "/alarms/clear", HTTP_POST, clearSchedules );
  server.on ( "/time/get", HTTP_GET, getClockTime );
  server.on ( "/time/set", HTTP_POST, setClockTime );
  server.on ( "/switch/1/set", HTTP_POST, toggleSwitchA );
  server.on ( "/switch/1/get", HTTP_GET, readSwitchA ); 
  server.on ( "/switch/2/set", HTTP_POST, toggleSwitchB );
  server.on ( "/switch/2/get", HTTP_GET, readSwitchB ); 
  server.on ( "/switch/get", HTTP_GET, readAllSwitches ); 
  server.on ( "/", handleRoot );
  server.onNotFound ( handleNotFound );
  
  server.begin();
  Log.notice("HTTP server started"CR);
}

/**
 * Handles root request
 */
void handleRoot()
{
  server.send(200, "text/plain", "hello"); 
}

/**
 * Handles 404 requests
 */
void handleNotFound()
{
  server.send(404, "text/plain", "Not found"); 
}

/**
 * Read state of switch A
 */
void readSwitchA()
{
  String data;
  
  StaticJsonBuffer<100> responseBuffer;
  RtcDateTime now = Rtc.GetDateTime();
  JsonObject& response = responseBuffer.createObject();
  response["id"] = conf.r1_id;
  response["state"] = conf.r1_state;
  response["lastupdate"] = conf.r1_updated;
  response.printTo(data);

  server.send(200, "application/json", data); 
}

/**
 * Toggle state of switch A
 */
void toggleSwitchA()
{
  String data;
  dtnow = Rtc.GetDateTime();
  
  if(conf.r1_state == 0)
  {
    conf.r1_state=1;
    conf.r1_updated = dtnow.Epoch32Time();
    relays_dirty = true;
  }
  else
  {
    conf.r1_state=0;
    conf.r1_updated = dtnow.Epoch32Time();
    relays_dirty = true;
  }

  StaticJsonBuffer<100> responseBuffer;  
  JsonObject& response = responseBuffer.createObject();
  response["id"] = conf.r1_id;
  response["state"] = conf.r1_state;
  response["lastupdate"] = conf.r1_updated;
  response.printTo(data);
  
  server.send(200, "application/json", data);
}

/**
 * Read state of switch B
 */
void readSwitchB()
{
  StaticJsonBuffer<100> jsonBuffer;
  RtcDateTime now = Rtc.GetDateTime();
  JsonObject& root = jsonBuffer.createObject();
  root["id"] = conf.r2_id;
  root["state"] = conf.r2_state;
  root["lastupdate"] = conf.r2_updated;

  String data;
  root.printTo(data);

  server.send(200, "application/json", data); 
}

/**
 * Toggle state of switch B
 */
void toggleSwitchB()
{
  String data;
  dtnow = Rtc.GetDateTime();
  
  if(conf.r2_state == 0)
  {
    conf.r2_state=1;
    conf.r2_updated = dtnow.Epoch32Time();
    relays_dirty = true;
  }
  else
  {
    conf.r2_state=0;
    conf.r2_updated = dtnow.Epoch32Time();
    relays_dirty = true;
  }

  StaticJsonBuffer<100> jsonBuffer;
  JsonObject& root = jsonBuffer.createObject();
  root["id"] = conf.r2_id;
  root["state"] = conf.r2_state;
  root["lastupdate"] = conf.r2_updated;
  
  root.printTo(data);

  server.send(200, "application/json", data);
}


/**
 * Reads all switches
 */
void readAllSwitches()
{
  DynamicJsonBuffer responseBuffer;
  
  JsonObject& response = responseBuffer.createObject();
  response["status"] = "success";
  
  JsonArray& items = response.createNestedArray("data");

  JsonObject& item1 = items.createNestedObject();
  item1["id"] = conf.r1_id;
  item1["state"] = conf.r1_state;
  item1["lastupdate"] = conf.r1_updated;

  JsonObject& item2 = items.createNestedObject();
  item2["id"] = conf.r2_id;
  item2["state"] = conf.r2_state;
  item2["lastupdate"] = conf.r2_updated;

  String data;
  response.printTo(data);

  server.send(200, "application/json", data);
}



/**
 * Read alarm schedules
 */
void getSchedules()
{
  Log.trace("getSchedules called"CR);
  
  String data;

  DynamicJsonBuffer responseBuffer;
  JsonObject& response = responseBuffer.createObject();
  
  // read from eeprom
  readSchedules(); 

  if(dat.strlength < 4){
    String content = ""; 
    content.toCharArray(dat.str, content.length());
  } 

  
  Log.notice("Preparing schedule data: %s"CR, dat.str);
    
  response["status"] = "success";
  
  JsonArray& items = response.createNestedArray("data");
  
  int itempos;
  char *tok, *sav1 = NULL;
  tok = strtok_r(dat.str, "|", &sav1);
  
  while (tok) 
  {
    JsonObject& item = items.createNestedObject();
    
    int sch_index;
    int sch_hh;
    int sch_mm;
    char sch_target[3];
    int sch_target_state;
    long sch_timestamp;
  
    char *subtok, *sav2 = NULL;
    subtok = strtok_r(tok, ":", &sav2);
    itempos = -1;
    
    while (subtok) 
    {
    itempos++;
    
    if(itempos == 0)
    {
      sscanf (subtok, "%d", &sch_index);
      item["o"] = sch_index;
    }
    else if(itempos == 1)
    {
      sscanf (subtok, "%d", &sch_hh);
      item["h"] = sch_hh;
    }
    else if(itempos == 2)
    {
      sscanf (subtok, "%d", &sch_mm);
      item["m"] = sch_mm;
    }
    else if(itempos == 3)
    {     
      item["d"] = subtok;
    }
    else if(itempos == 4)
    {
      item["tr"] = subtok;
    }
    else if(itempos == 5)
    {
      sscanf (subtok, "%d", &sch_target_state);
      item["st"] = sch_target_state;
    }
    else if(itempos == 6)
    {
      sch_timestamp = strtol(subtok,NULL,10);
      item["ts"] = sch_timestamp;
    }      
      
    subtok = strtok_r(NULL, ":", &sav2);
    }
  
    tok = strtok_r(NULL, "|", &sav1);
  }

  response.printTo(data);
  server.send(200, "application/json", data); 
}


/**
 * Get RTC Time
 */
void getClockTime()
{
  Log.trace("getClockTime called"CR);
  
  String data;
  
  StaticJsonBuffer<100> responseBuffer;
  RtcDateTime now = Rtc.GetDateTime();
  printDateTime(now);
  JsonObject& response = responseBuffer.createObject();
  response["h"] = now.Hour();
  response["i"] = now.Minute();
  response["s"] = now.Second();
  response["dd"] = now.Day();
  response["mm"] = now.Month();
  response["yy"] = now.Year();
  response.printTo(data);

  server.send(200, "application/json", data); 
}


/*
 * Clears all schedule data from eeprom
 */
void clearSchedules()
{
  Log.trace("clearSchedules called"CR);

  String data;
  StaticJsonBuffer<100> outBuffer;
  JsonObject& response = outBuffer.createObject();

  boolean result = EEPROM.wipe();
  if (result) {    
    memset(dat.str, 0, sizeof(dat.str));
    dat.strlength = 0;
    writeSchedules();
    Log.notice("All EEPROM data wiped"CR);
    schedules_updated = true;
  } else {
    Log.notice("EEPROM data could not be wiped from flash store"CR);
  } 
      
  if (!result)
  {
    response["status"] = "error";
  }
  else
  {
    response["status"] = "success";  
  }  

  response.printTo(data);
  server.send(200, "application/json", data);
}


/**
 * Set alarm schedules
 */
void setSchedules()
{
  Log.trace("setSchedules called"CR);
  
  String data;
  StaticJsonBuffer<100> outBuffer;
  JsonObject& response = outBuffer.createObject();
      
  if (server.hasArg("plain")== false)
  {
    response["status"] = "error";
    response.printTo(data);
    server.send(400, "application/json", data);
  }
  else
  {
      DynamicJsonBuffer inBuffer;
      JsonObject& inObj = inBuffer.parseObject(server.arg("plain"));
      //JsonObject& inObj = inBuffer.parseObject(raw);
      
      // Test if parsing succeeds.
      if (!inObj.success()) {
          response["status"] = "error";
          response.printTo(data);
          server.send(400, "application/json", data);
      }     

      // get schdules
      JsonArray& items = inObj["data"];
      
      // verify that we have less than max schedules
      if(items.size() > MAX_SCHEDULES){
          response["status"] = "error";
          response["message"] = "Requested number of schedules exceeds max allowed!";
          response.printTo(data);
          Log.notice("Requested number of schedules exceeds max allowed!: %s"CR);
          server.send(400, "application/json", data);
      }

      String content = jsonToSchedules(items);    
      content.toCharArray(dat.str, content.length());
      
      //serialize schedules to eeprom      
      writeSchedules();

      response["status"] = "success";
      response.printTo(data);
      server.send(200, "application/json", data);
  }  
}


/**
 * Converts schedules string to json
 */
String jsonToSchedules(JsonArray& items)
{
  String schedules = "";
  int index = 0;
  for (auto& item : items) {
    int o = item["o"];
    int h = item["h"];
    int m = item["m"];
    const char* d = item["d"];
    const char* tr = item["tr"];
    int st = item["st"];
    long ts = item[String("ts")];
    if(ts == 0){
      ts = 1550327253;
    }

    schedules = schedules + o + ":" + h + ":" + m + ":" + d + ":" + tr + ":" + st + ":" + ts; // schedule string
    index++;

    if(index < items.size()){
      schedules = schedules + "|"; // boundary
    }
  }

  return schedules;
}



/**
 * Sets RTC time
 */
void setClockTime()
{  
  Log.trace("setClockTime called"CR);
  
  String data;

  StaticJsonBuffer<100> responseBuffer;
  RtcDateTime dt = Rtc.GetDateTime();
  
  if (server.hasArg("plain")== false){
      // Read request data
      StaticJsonBuffer<200> requestBuffer;
      JsonObject& request = requestBuffer.parseObject(server.arg("plain"));
      int yy = request["yy"];
      int mm = request["mm"];
      int dd = request["dd"];
      int h = request["h"];
      int i = request["i"];
      int s = request["s"];
    
      //RtcDateTime(uint16_t year, uint8_t month, uint8_t dayOfMonth, uint8_t hour, uint8_t minute, uint8_t second)
      RtcDateTime currentTime = RtcDateTime(yy,mm,dd,h,i,s); //define date and time object
      Rtc.SetDateTime(currentTime);
    }  

    // Prepare response data
    JsonObject& response = responseBuffer.createObject();
    response["status"] = "success";
    response["data"] = "";
    response.printTo(data);

    server.send(200, "application/json", data);
}

/**
 * Initialize RTC
 */
void setupRTC()
{
    Log.trace("compiled date : %s time : %s"CR, __DATE__, __TIME__);
    Rtc.Begin();

    RtcDateTime compiled = RtcDateTime(__DATE__, __TIME__);
    printDateTime(compiled);
    Log.trace("======================================"CR);

    if (!Rtc.IsDateTimeValid()) 
    {
        Log.notice("RTC lost confidence in the DateTime!"CR);
        Rtc.SetDateTime(compiled);
    }

    if (!Rtc.GetIsRunning())
    {;
        Log.notice("RTC was not actively running, starting now!"CR);
        Rtc.SetIsRunning(true);
    }

    dtnow = Rtc.GetDateTime();
    if (dtnow < compiled) 
    {
        Log.notice("RTC is older than compile time!  (Updating DateTime)"CR);
        Rtc.SetDateTime(compiled);
    }
    else if (dtnow > compiled) 
    {
        Log.notice("RTC is newer than compile time. (this is expected)"CR);
    }
    else if (dtnow == compiled) 
    {
        Log.notice("RTC is the same as compile time! (not expected but all is fine)"CR);
    }

    // just clear them to your needed state
    Rtc.Enable32kHzPin(false);
    Rtc.SetSquareWavePin(DS3231SquareWavePin_ModeNone); 

    setSyncProvider(syncProvider);   // the function to get the time from the RTC
    if(timeStatus()!= timeSet) 
    Log.notice("Unable to sync with the RTC"CR);
    else
    Log.notice("RTC has set the system time"CR);
}


time_t syncProvider()
{
  dtnow = Rtc.GetDateTime();
  return dtnow.Epoch32Time();
}



/**
 * Parse schedule string and collect schedules in an array
 */
void collectSchedule()
{
  Log.trace("Collecting schedules"CR);
  
  // clear array
  memset(&user_schedules[0], 0, sizeof(user_schedules));
   
  Log.trace("Schedule data : %s"CR, dat.str);
  
  int itempos;
  char *tok, *sav1 = NULL;
  tok = strtok_r(dat.str, "|", &sav1);

  // collect schedules
  counter=0;
  while (tok) 
  {
      Log.trace("tok : %s"CR, tok);
      
      int sch_index;
      int sch_hh;
      int sch_mm;
      int sch_days[6];
      int sch_total_days;
      char sch_target[3];
      int sch_target_state;
      long sch_timestamp;
    
      char *subtok, *sav2 = NULL;
      subtok = strtok_r(tok, ":", &sav2);
      itempos = -1;
      
      while (subtok) 
      {
        itempos++;
        
        if(itempos == 0)
        {
          sscanf (subtok, "%d", &sch_index);
        }
        else if(itempos == 1)
        {
          sscanf (subtok, "%d", &sch_hh);
        }
        else if(itempos == 2)
        {
          sscanf (subtok, "%d", &sch_mm);
        }
        else if(itempos == 3)
        {
          int total_days = countChars( subtok, ',' ) + 1;
          int days[total_days];
          int j = 0;
          char *toki, *savi = NULL;
          toki = strtok_r(subtok, ",", &savi);
          while (toki) 
          {
            int d;   
            sscanf (toki, "%d", &d);            
            sch_days[j] = d;
            j++;
            
            toki = strtok_r(NULL, ",", &savi);
          }

          sch_total_days = total_days;
        }
        else if(itempos == 4)
        {
          my_strcpy(sch_target, subtok);
        }
        else if(itempos == 5)
        {
          sscanf (subtok, "%d", &sch_target_state);
        }
        else if(itempos == 6)
        {
          sch_timestamp = strtol(subtok,NULL,10);
        }      
          
        subtok = strtok_r(NULL, ":", &sav2);
      }


      // break into individual schedules
      for(int k=0;k<sch_total_days;k++)
      {
        ScheduleItem item;
        int dow = sch_days[k];

        item.parent_index = sch_index;
        item.index = counter;
        item.hh = sch_hh;
        item.mm = sch_mm;
        item.dow = dow;
        my_strcpy(item.target, sch_target); 
        item.target_state = sch_target_state;
        item.reg_timestamp = sch_timestamp;
        item.status = 1;
                
        user_schedules[counter] = item; 
        counter++;
      }
      
      tok = strtok_r(NULL, "|", &sav1);
  }

  Log.notice("Total of %d items collected"CR, counter);
}



/*
 * Evaluate schedules
 */
void evaluate()
{
  if (!Rtc.IsDateTimeValid()) 
  {
      Log.trace("RTC lost confidence in the DateTime!"CR);
  }
  else
  {
    if(millis() - lastEval > 500)
    {
      lastEval = millis();
      //dtnow = Rtc.GetDateTime();
      dtnow = RtcDateTime(604808986);
      ScheduleItem sch1  = getNearestPastSchedule(dtnow, "s1");
      //toString(sch1);
    }
  }
}



/**
 * Sort schedules list
 */
void sortSchedule()
{
    /*
    Log.trace("Before"CR);
    for(int i=0;i<counter;i++){
      ScheduleItem item = user_schedules[i];
      //toString(item);
    }
    */
    
    qsort((void *) &user_schedules, counter, sizeof(struct ScheduleItem), (compfn)compare );

    /*
    Log.trace("After"CR);
    for(int i=0;i<counter;i++){
      ScheduleItem item = user_schedules[i];
      //toString(item);
    }
    */
}


/*
 * qsort timewise sorting of schedules
 */
int compare(struct ScheduleItem *elem1, struct ScheduleItem *elem2)
{
    // check by reg timestamp

    // check by dow

    // check by time

    if ( elem1->dow == elem2->dow)
    {
      if(elem1->hh == elem2->hh)
      {
        if(elem1->mm == elem2->mm)
        {
          return elem1->parent_index - elem2->parent_index;
        }
        else
        {
          return elem1->mm - elem2->mm;
        }
      }
      else
      {
        return elem1->hh - elem2->hh;
      }
    }
    else
    {
      return elem1->dow - elem2->dow;
    }
}


/**
 * Find nearest past schedule for a relay with respect to today
 */
struct ScheduleItem getNearestPastSchedule(RtcDateTime& dt, char* target)
{
  Log.trace("looking for nearest past schedule"CR);

  const int APPLICABLE_SCHEDULES_COUNT = counter;
  ScheduleItem candidate;
  
  ScheduleItem applicable_schedules[APPLICABLE_SCHEDULES_COUNT];
  int i = counter;
  int j = 0;

  /*
   * Collect valid candidate schedules
   */
   
  while(i > 0)
  {
    ScheduleItem sch = user_schedules[i];

    // collect scheudles for requested target only
    if (strcmp(sch.target, target) == 0)
    {      
      if(getDayDiff(dt, sch) >= getDowDiff(dt,sch))
      {
        if(isPastTime(dt, sch))
        {
          applicable_schedules[j] = sch;
          j++;
        }
      }      
    }
    
    i--;
  }

   /*
   * Sort collected schedules
   */
    if(j>0)
    {
      /*
      Log.trace("Before"CR);
      for(int k=0;k<j;k++){
      ScheduleItem sample = applicable_schedules[k];
      toString(sample);
      }
      */
      
      qsort((void *) &applicable_schedules, counter, sizeof(struct ScheduleItem), (compfn)nearestPast );      
      candidate = applicable_schedules[0];

      /*
      Log.trace("After"CR);
      for(int k=0;k<j;k++){
      ScheduleItem sample = applicable_schedules[k];
      toString(sample);
      }
      */
    }
    else
    {
      candidate.parent_index = -1;
      candidate.hh = -1;
      candidate.mm = -1;
      candidate.dow = -1;
      candidate.reg_timestamp = -1;
      candidate.target_state = -1;
    }

    /*
    * Clear array
    */
    memset(&applicable_schedules[0], 0, sizeof(applicable_schedules));


    /*
    * Apply best match schedule
    */
       
   return candidate;
}


/**
 * Compare to sort nearest past schedule
 */
int nearestPast(const void *a, const void *b)
{
  struct ScheduleItem *x = (struct ScheduleItem *)a;
  struct ScheduleItem *y = (struct ScheduleItem *)b;
  
  /* cast pointers to adjacent elements to struct ScheduleItem* */
    /*struct ScheduleItem *x = a, *y = b;*/

    /* (x->time < y->time) - (x->time > y->time) - descending sort
     * comparison avoids potential overflow
     */
    if (x->reg_timestamp != y->reg_timestamp)
        return (x->reg_timestamp < y->reg_timestamp) - 
               (x->reg_timestamp > y->reg_timestamp);
    /* compare dow next */
    else if (x->dow != y->dow)
        return (x->dow < y->dow) - (x->dow > y->dow);
    /* compare hh next */
    else if (x->hh != y->hh)
        return (x->hh < y->hh) - (x->hh > y->hh);
    /* finally compare mm */
    else
        return (x->mm < y->mm) - (x->mm > y->mm);
}


/**
 * Get absolute difference between current day of week and schedule's day of week
 */
int getDowDiff(RtcDateTime& dt, ScheduleItem& t2)
{ 
    return abs(dt.DayOfWeek() - t2.dow);
}


/**
 * Get difference between current date and schedule date in `days`
 */
int getDayDiff(RtcDateTime& dt, ScheduleItem& t2)
{
  return (((dt.Epoch32Time() - t2.reg_timestamp) /60)/60)/24;
}


/**
 * Check if schedule time is before current time
 */
boolean isPastTime(RtcDateTime& dt, ScheduleItem& t2)
{
  if(t2.dow < dt.DayOfWeek())
  {
      return true;
  }
  else
  {
      if(t2.hh < dt.Hour())
      {
        return true;
      }
      else if(t2.hh == dt.Hour())
      {
        if(t2.mm <= dt.Minute())
        {
          return true;
        }
      }
  }
  
  return false;
}



/**
 * Print date time from RTC
 */
void printDateTime(const RtcDateTime& dt)
{
    char datestring[20];

    snprintf_P(datestring, 
            countof(datestring),
            PSTR("%02u/%02u/%04u %02u:%02u:%02u"),
            dt.Month(),
            dt.Day(),
            dt.Year(),
            dt.Hour(),
            dt.Minute(),
            dt.Second() );
    Log.notice("%s"CR, datestring);
}



/*
 * Writes schedule data to eeprom
 */
void writeSchedules()
{
  Log.notice("Writing schedules data. %s"CR, dat.str); 
  
  EEPROM.put(SCHEDULES_START_ADDR, dat);
  boolean ok = EEPROM.commit();

  if(ok){
    Log.notice("Commit OK"CR);
    schedules_updated = true;
  }else{
    Log.notice("Commit failed"CR);
  }
}


/**
 * Reads schedule data
 */
void readSchedules()
{
  Log.notice("Reading schedules data."CR);
  EEPROM.get(SCHEDULES_START_ADDR, dat);
  Log.notice("schedules data. %s"CR, dat.str);
}


/**
 * Save relay states
 */
void writeRelays()
{
  Log.notice("Writing relay states."CR); 
  
  EEPROM.put(EEPROM_START_ADDR, conf);
  boolean ok = EEPROM.commit();

  if(ok){
    Log.notice("Commit OK"CR);
  }else{
    Log.notice("Commit failed"CR);
  }
}


/**
 * Reads schedule data
 */
void readRelays()
{
  Log.notice("Reading relays data."CR);
  EEPROM.get(EEPROM_START_ADDR, conf);
}


/*
 * Erases EEPROM settingby writing 0 in each byte
 */
void eraseSettings()
{
  Log.notice("Erasing eeprom..."CR);
  
  for (int i = 0; i < EEPROM_MAX_LIMIT; i++)
    EEPROM.write(i, 0);
}


/*
 * Count characters
 */
int countChars( char* s, char c )
{
    return *s == '\0'
              ? 0
              : countChars( s + 1, c ) + (*s == c);
}


/*
 * Copies one char array to another
 */
char *my_strcpy(char *destination, char *source)
{
    char *start = destination;
 
    while(*source != '\0')
    {
        *destination = *source;
        destination++;
        source++;
    }
 
    *destination = '\0'; // add '\0' at the end
    return start;
}


/**
 * Schedule toString method
 */
void toString(ScheduleItem item)
{
  Log.trace("Index : %d, Hour: %d, Minute: %s, DayOfWeek: %d, Reg Timestamp: %d"CR, item.index, item.hh, item.mm, item.dow, item.reg_timestamp);
}
