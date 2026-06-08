# esp32 display Project

The esp32 display system is an ESP32-c3 and a GMT028 tft 2.8" with ILI9341 chip, to display informations on the display.

## Main Function

The esp should have a http server that accepts connections from my home network to be configured. should display informations like the weather, bitcoin price, eth price and litecoin price... or any coin I configure. whould also display time and date. in the future should be integrated to calendar.

The ESP32 should use OTA flashing and logging. 

## Implementation Phases

1. design the circuit 
2. Implement all ESP32 functions. In this phase, we want to debug the infrastructure, such as OTA flashing and debugging.
3. Implement the http server that allows the project configuration

## Tools

Use ESP-IDF and esptool 


Create a new GitHub Repository on https://github.com/andrerung/esp32DisplayProject

