
/* 
 *  SigGen by Jeremy Madea
 *  
 *  Based on code originally written by Richard Visokey AD7C - www.ad7c.com 
 *  and Todd Gruener K7KXI 
 *  
 *  Links for the non-standard libraries used herein: 
 *  https://github.com/marcoschwartz/LiquidCrystal_I2C.git
 *  https://github.com/buxtronix/arduino/tree/master/libraries/Rotary
 *  
 * 
 */

// Include the library code
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Rotary.h>
#include <EEPROM.h>

#define MAX_FREQ 50000000

// AD9850 Pins
#define AD9850_WCLK 8   // Pin 8  - Word Load Clock  (WCLK)
#define AD9850_FQUD 9   // Pin 9  - Frequency Update (FQUP)
#define AD9850_DATA 10  // Pin 10 - Serial Data Load (DATA/D7)
#define AD9850_RST  11  // Pin 11 - Reset (RST)

// Input Pins
#define ENC_PIN_A 2
#define ENC_PIN_B 3
#define ENC_BTN   A0

// Convenience macros
#define pulse_high(pin) { digitalWrite(pin, HIGH); digitalWrite(pin, LOW); }

// LCD constants

    /*
     * I2C_ADDR -  The address of the PCF8574x-based LCD I2C Backpack. 
     *             ** CHECK YOUR MODULE. THEY AREN"T ALL THE SAME. ** 
     */
#define I2C_ADDR      0x27  // Address for the LCD I2C Backpack. (PCF8574) 


// UI Constants 
#define UI_CHANGE_FREQUENCY 0
#define UI_CHANGE_INCREMENT 1

#define MEM_STALE 0
#define MEM_FRESH 1
#define MEM_UPDATE_INTERVAL 2000 // milliseconds between writes to eeprom. 

// Global objects for rotary encoder and LCD. 
Rotary r = Rotary(ENC_PIN_A, ENC_PIN_B); // sets the pins the rotary encoder uses.  Must be interrupt pins.
LiquidCrystal_I2C lcd(I2C_ADDR, 16, 2); 

// Variables needed to track the frequency. 
int_fast32_t rx=1000;          // Desired output frequency. (i.e. changed by user.)  
int_fast32_t rx2=1;            // Current output frequency. 

// Vartiables needed to track and display the increment. 
int_fast32_t increment = 1000; // starting VFO update increment in Hz.


// Accumulator for the rotary encoder. 
int_fast8_t encoder_change = 0; 

// Variables needed for LCD display
byte ones, tens, hundreds, thousands, tenthousands, hundredthousands, millions;
String freq;
String inc_display_string;       //  = "10 Hz";
int  inc_display_position; //  = 5;

int_fast32_t ad9850_update_timestamp = 0;

int mem_status = MEM_FRESH; 
byte ui_mode   = UI_CHANGE_FREQUENCY;


void setup() {

  // The encoder button shoud connect the ENC_BTN pin to GND when pushed. 
  pinMode(ENC_BTN, INPUT);
  digitalWrite(ENC_BTN, HIGH);

  // Set up the interrupt for the rotary encoder.  
  PCICR |= (1 << PCIE2);
  PCMSK2 |= (1 << PCINT18) | (1 << PCINT19);
  sei();

  // Initialize the AD9850
  pinMode(AD9850_FQUD, OUTPUT);
  pinMode(AD9850_WCLK, OUTPUT);
  pinMode(AD9850_DATA, OUTPUT);
  pinMode(AD9850_RST,  OUTPUT);
   
  pulse_high( AD9850_RST );
  pulse_high( AD9850_WCLK );
  pulse_high( AD9850_FQUD );  // this pulse enables serial mode on the AD9850 - Datasheet page 12.

  // Get our saved state. 
  // If our saved increment is a power of 10, we assume we saved it. Otherwise, 
  // we assume this is a first boot and we use defaults. It's possible we could 
  // find a power of ten even though we didn't save it... so in a rare instance 
  // we might end up with a random frequency on start-up. I'm okay with this. 
  eeprom_load_state(); 
  if ( ! is_power_of_ten( increment )) {
    rx = 1000; 
    increment = 1; 
  }

  // Start the AD9850
  ad9850_update_frequency(rx);
  
  // This is a hack to get the inc_display_string and inc_display_position variables set correctly. 
  increase_increment(); 
  decrease_increment(); 

  // Initialize the LCD. 
  lcd.init();
  lcd.home(); 
  display_frequency(); 
  display_increment();
  display_indicator();
  lcd.backlight();

}


void loop() {

  if (encoder_change != 0) { 
    
    if (ui_mode == UI_CHANGE_FREQUENCY) {
       
      if (encoder_change > 0) {
        do { 
          increase_frequency(); 
          encoder_change--; 
        } while (encoder_change > 0);  
      } else { 
        do { 
          decrease_frequency(); 
          encoder_change++; 
        } while (encoder_change < 0);        
      }

      // If our frequency really changed, we need to 
      // display the new frequency and update the AD9850. 
      if (rx != rx2) { 
        display_frequency();
        ad9850_update_frequency(rx);
        rx2 = rx;
        mem_status = MEM_STALE;
        display_indicator();  
      }
            
    }
    else if (ui_mode == UI_CHANGE_INCREMENT) {
       
      if (encoder_change > 0) {
        do { 
          increase_increment(); 
          encoder_change--; 
        } while (encoder_change > 0);  
      } else { 
        do { 
          decrease_increment(); 
          encoder_change++; 
        } while (encoder_change < 0);        
      }
      
      display_increment(); 
      display_indicator();
    }
  }

  // Poll the button. If it is pressed, we need to change modes. 
  if (digitalRead(ENC_BTN) == LOW) {
    ui_mode = ui_mode ^ 0x1; // Only two modes (0, 1) for now, so we just toggle.
    display_indicator(); 
    delay(250);              
  };

  // Write the frequency to memory if not stored and 2 seconds have passed since the last frequency change.
   if (mem_status == MEM_STALE) {
    if ( millis() - ad9850_update_timestamp > MEM_UPDATE_INTERVAL ) 
        eeprom_save_state();  
   }
}



// Interrupt handler checks the encoder accumulates the change. 
ISR(PCINT2_vect) {
  unsigned char result = r.process();
  if (result == DIR_CW)  encoder_change ++; 
  if (result == DIR_CCW) encoder_change --; 
}



/*   
 *  Functions for communicating with the AD9850 
 *  
 **/


// Sends a byte, one bit at a time, LSB first to the AD9850 via serial DATA line
void ad9850_send_byte( byte data ) {
  for (int i=0; i<8; i++, data>>=1) {
    digitalWrite(AD9850_DATA, data & 0x01);
    pulse_high(AD9850_WCLK);   //after each bit sent, CLK is pulsed high
  }
}


// Function to show whether we are changing frequency or increment. 
inline void display_indicator() { 
  lcd.setCursor(0, ui_mode ^ 0x1); 
  lcd.print(" "); 
  lcd.setCursor(0, ui_mode); 
  lcd.print(">"); 
}



/*   
 *  Functions for changing and displaying the frequency 
 *  
 **/

// frequency calc from datasheet page 8 = <sys clock> * <frequency tuning word>/2^32
void ad9850_update_frequency(double frequency) {
    
  // The AD9850 has a 125 MHz clock. 
  // You can adjust the clock frequency to fine tune the AD9850 output frequency.   
  int32_t freq = frequency * 4294967295 / 125000000;

  // Send the bits to the AD9850. It is looking for 40 bits (5 bytes)  
  for (int b=0; b<4; b++, freq>>=8) {
    ad9850_send_byte(freq & 0xFF);
  }
  ad9850_send_byte(0x000);   // Final byte is 0. 
  pulse_high(AD9850_FQUD);   // Done!
  ad9850_update_timestamp = millis(); // record the time. 
}

void increase_frequency() { 
  rx += increment; 
  if (rx > MAX_FREQ) rx = MAX_FREQ; 
}

void decrease_frequency() { 
  rx -= increment; 
  if (rx < 1) rx = 1; 
}

void display_frequency() {
  millions         = int( rx / 1000000 );
  hundredthousands = ( (rx / 100000) % 10);
  tenthousands     = ( (rx / 10000)  % 10);
  thousands        = ( (rx / 1000)   % 10);
  hundreds         = ( (rx / 100)    % 10);
  tens             = ( (rx / 10)     % 10);
  ones             = ( rx            % 10);
    

  lcd.setCursor(0,0);
  lcd.print("                ");

  if (millions > 9){
    lcd.setCursor(1,0);
  }
  else {
    lcd.setCursor(2,0);
  }
  lcd.print(millions);
  lcd.print(".");
  lcd.print(hundredthousands);
  lcd.print(tenthousands);
  lcd.print(thousands);
  lcd.print(".");
  lcd.print(hundreds);
  lcd.print(tens);
  lcd.print(ones);
  lcd.print(" MHz");
};



/*   
 *  Functions for changing and displaying the increment 
 *  
 **/

void display_increment() { 
  lcd.setCursor(0,1);
  lcd.print("                ");
  lcd.setCursor(inc_display_position,1); 
  lcd.print(inc_display_string); 
}


void increase_increment() {
  if      (increment == 1)       { increment = 10;       inc_display_string=  "10 Hz";  inc_display_position = 5; }
  else if (increment == 10)      { increment = 100;      inc_display_string= "100 Hz";  inc_display_position = 4; }
  else if (increment == 100)     { increment = 1000;     inc_display_string=   "1 kHz"; inc_display_position = 6; }    
  else if (increment == 1000)    { increment = 10000;    inc_display_string=  "10 kHz"; inc_display_position = 5; }
  else if (increment == 10000)   { increment = 100000;   inc_display_string= "100 kHz"; inc_display_position = 4; }
  else if (increment == 100000)  { increment = 1000000;  inc_display_string=   "1 Mhz"; inc_display_position = 6; }
  else if (increment == 1000000) { increment = 10000000; inc_display_string=  "10 Mhz"; inc_display_position = 5; }  
  else                           { increment = 1;        inc_display_string=   "1 Hz";  inc_display_position = 6; }
};

void decrease_increment() {
  if      (increment == 1)        { increment = 10000000; inc_display_string=  "10 MHz"; inc_display_position = 5; }
  else if (increment == 10)       { increment = 1;        inc_display_string=   "1 Hz";  inc_display_position = 6; }  
  else if (increment == 100)      { increment = 10;       inc_display_string=  "10 Hz";  inc_display_position = 5; }
  else if (increment == 1000)     { increment = 100;      inc_display_string= "100 Hz";  inc_display_position = 4; }
  else if (increment == 10000)    { increment = 1000;     inc_display_string=   "1 kHz"; inc_display_position = 6; }    
  else if (increment == 100000)   { increment = 10000;    inc_display_string=  "10 kHz"; inc_display_position = 5; }
  else if (increment == 1000000)  { increment = 100000;   inc_display_string= "100 kHz"; inc_display_position = 4; }
  else if (increment == 10000000) { increment = 1000000;  inc_display_string=   "1 MHz"; inc_display_position = 6; }  
  else                            { increment = 1;        inc_display_string=   "1 Hz";  inc_display_position = 6; }
};



/* 
 *  Functions for saving/loading application state to/from the eeprom. 
 *  
 **/

void eeprom_save_state() {
    int address = 0;   

    EEPROM.put(address, rx); 
    address += sizeof( int_fast32_t ); 
    EEPROM.put(address, increment); 
    mem_status = MEM_FRESH;
}

void eeprom_load_state() { 
    int address = 0; 
    EEPROM.get(address, rx);
    address += sizeof( int_fast32_t );
    EEPROM.get(address, increment);
    mem_status = MEM_FRESH;   
}

inline bool is_power_of_ten(int x) {
  while ( (x > 9) && (x % 10 == 0) ) 
    x /= 10;
  return (x == 1); 
}


