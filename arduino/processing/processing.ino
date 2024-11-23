#include <ArduinoJson.h>
#include <stdlib.h>
#include <TimeLib.h>
#include <stdio.h>


/* for normal hardware wire use below */
#include <Wire.h>
#include <RtcDS3231.h>
RtcDS3231<TwoWire> Rtc(Wire);
/* for normal hardware wire use above */

typedef int (*compfn)(const void*, const void*);
char *my_strcpy(char *destination, char *source);

int eeAddress = 0;
boolean debug = true;
int counter;

const String processed = "1:03:15:0,1,2,3,4,5:s1:0:1550951769|2:20:30:0,1,2,3:s1:1:1550951769";
const int MAX_SCHEDULES = 20;
const int EEPROM_MAX_LIMIT = 512;
const int EEPROM_START_ADDR = 0;

RtcDateTime dtnow;

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


ScheduleItem user_schedules[MAX_SCHEDULES] = {};

void setupRTC()
{
    Serial.print("compiled: ");
    Serial.print(__DATE__);
    Serial.println(__TIME__);
    
    Rtc.Begin();

    RtcDateTime compiled = RtcDateTime(__DATE__, __TIME__);
    printDateTime(compiled);
   
    Serial.println();

    if (!Rtc.IsDateTimeValid()) 
    {
        Serial.println("RTC lost confidence in the DateTime!");
        Rtc.SetDateTime(compiled);
    }

    if (!Rtc.GetIsRunning())
    {
        Serial.println("RTC was not actively running, starting now");
        Rtc.SetIsRunning(true);
    }

    RtcDateTime now = Rtc.GetDateTime();
    if (now < compiled) 
    {
        Serial.println("RTC is older than compile time!  (Updating DateTime)");
        Rtc.SetDateTime(compiled);
    }
    else if (now > compiled) 
    {
        Serial.println("RTC is newer than compile time. (this is expected)");
    }
    else if (now == compiled) 
    {
        Serial.println("RTC is the same as compile time! (not expected but all is fine)");
    }

    // never assume the Rtc was last configured by you, so
    // just clear them to your needed state
    Rtc.Enable32kHzPin(false);
    Rtc.SetSquareWavePin(DS3231SquareWavePin_ModeNone); 
}

void setup() 
{
  Serial.begin(57600);
  
  setupRTC();  

  collectSchedule();
  sortSchedule();
  
  evaluate();
}


void loop() { 
  
}


#define countof(a) (sizeof(a) / sizeof(a[0]))

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
    Serial.print(datestring);
}

void evaluate()
{
  if (!Rtc.IsDateTimeValid()) 
  {
      Serial.println("RTC lost confidence in the DateTime!");
  }
  else
  {
    //dtnow = Rtc.GetDateTime();
    dtnow = RtcDateTime(604808986);
    ScheduleItem sch1  = getNearestPastSchedule(dtnow, "s1");
    toString(sch1);
  }
}


/**
 * Find nearest past schedule for a relay with respect to today
 */
struct ScheduleItem getNearestPastSchedule(RtcDateTime& dt, char* target)
{
  Serial.println("looking for nearest past schedule");

  ScheduleItem candidate;
  ScheduleItem applicable_schedules[MAX_SCHEDULES] = {};
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
      for(int k=0;k<j;k++){
      ScheduleItem sample = applicable_schedules[k];
      Serial.println(sample.dow);
      delay(10);
      }

      Serial.println("after");
      qsort((void *) &applicable_schedules, counter, sizeof(struct ScheduleItem), (compfn)nearestPast );      
      candidate = applicable_schedules[0];

      for(int k=0;k<j;k++){
      ScheduleItem sample = applicable_schedules[k];
      Serial.println(sample.dow);
      delay(10);
      }
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
  /* cast pointers to adjacent elements to struct ScheduleItem* */
    struct ScheduleItem *x = a, *y = b;

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
 * Sort schedules list
 */
void sortSchedule()
{
    //debugPrint("before");
  
    for(int i=0;i<counter;i++){
      ScheduleItem item = user_schedules[i];
      //toString(item);
    }
    
    qsort((void *) &user_schedules, counter, sizeof(struct ScheduleItem), (compfn)compare );

    //debugPrint("after");

    for(int i=0;i<counter;i++){
      ScheduleItem item = user_schedules[i];
      //toString(item);
    }
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
 * Parse schedule string and collect schedules in an array
 */
void collectSchedule()
{
  debugPrint("collecting");
  
  // clear array
  memset(&user_schedules[0], 0, sizeof(user_schedules));
   
  char sch[processed.length()+1];
  processed.toCharArray(sch, processed.length()+1);
  debugPrint(processed);
  
  int itempos;
  char *tok, *sav1 = NULL;
  tok = strtok_r(sch, "|", &sav1);

  // collect schedules
  counter=0;
  while (tok) 
  {
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

  debugPrint(String(counter) + "items");
}



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
  Serial.println(String(item.index) + ":" + String(item.hh) + ":" + String(item.mm) + ":" + String(item.dow) + ":" + String(item.reg_timestamp));
}


/**
 * printing
 */
void debugPrint(String message) {
  if (debug) {
    Serial.println(message);
  }
}


int countChars( char* s, char c )
{
    return *s == '\0'
              ? 0
              : countChars( s + 1, c ) + (*s == c);
}
