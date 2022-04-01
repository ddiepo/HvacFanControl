# HvacFanControl


This is a custom software for my personal desire to optimize the functioning of my furnace, with two main goals:

1. Run the blower fan longer after a heat cycle than the furnace control board allows.
2. Turn the ceiling fans to a higher speed during a heat cycle.

The first goal is pretty univeral.  My furnace has controls to allow a 60, 90, or 120 seconds of blower run time at the end of a heat cycle.  I found, however, that the heat exchanger and ductwork still had warm air beyond that, as well as extending the runtime helped to equalize the temperature throughout the house.

The second goal is more specific.  Because I have 10ft ceilings and the ductwork is in the ceiling, getting the hot air where we want it is a challenge.  With wifi controllable ceiling fans I'm able now to adjust the fan speed up during a heat cycle to push the warm air coming in from the ceiling ducts down to the rest of the room.  We don't want the extra breeze the rest of the time, so we set the fan speed back down.

I have one of the early wifi thermostats, originally 3M but now Radio Thermostat.  It has an open published API which makes it easy to poll for info on the thermostat and change the thermostat settings, including adjusting the blower fan between auto, circulate, and on.  This program identifies the end of a heat cycle then adjusts the blower to "on" for a few minutes, then restores to the previous state.

For ceiling fans, I have Modern Forms fans.  The API is not yet published, however others have reverse engineered the API and written tools to control the fans.  I used those sources to identify the API call needed to set the fan speed.  I have also disucssed with Modern Forms, and they have told me they intend to publish the API soon.  The program purposely waits a short time after the start of a heat cycle to adjust the fan speeds higher, allowing time for heat to enter the room.  It then turns the fans back down shortly after the heat cycle.

I picked c++  for this, partly because I had not been programming in this language for a while, but also I was looking for a small efficient application since it runs continuously.  I was looking to avoid the bloat associated with other modern interpreted languages.

## Dependencies
Requires libcurl and c++17 compiler

## Build
Because everything is contained in this one file, building is trivial:
```
g++ fan_controller.cpp -lcurl -std=c++17
```


