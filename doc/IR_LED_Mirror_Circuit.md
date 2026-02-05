# IR LED Mirror Circuit

## Overview

This circuit drives two infrared LEDs for mirroring meter readings to other devices. The IR LEDs are driven by GPIO pin D1 (GPIO5) on the ESP8266 D1 Mini via a single NPN transistor switch.

## Circuit Schematic

```
                      USB 5V
                        |
            +-----------+-----------+
            |                       |
           [R1]                    [R2]
           47Ω                     47Ω
            |                       |
            v                       v
          LED1                    LED2
         (IR 940nm)              (IR 940nm)
            |                       |
            +-----------+-----------+
                        |
                        C (Collector)
                        |
                      |/
         D1 ---[R3]---|    Q1 (2N2222)
        (GPIO5) 1kΩ   |\
                        |
                        E (Emitter)
                        |
                       GND
```

**How it works:**
1. D1 (GPIO5) outputs the serial data (active LOW due to inverted SoftwareSerial)
2. When D1 goes HIGH → transistor turns ON → LEDs light up
3. When D1 goes LOW → transistor turns OFF → LEDs turn off
4. The transistor acts as a simple switch, allowing the 3.3V GPIO to control 5V/100mA LEDs

## Parts List

| Part | Value | Description | Quantity |
|------|-------|-------------|----------|
| Q1 | 2N2222 | NPN transistor (or BC547, 2N3904) | 1 |
| R1, R2 | 47Ω 1/4W | LED current limiting resistors | 2 |
| R3 | 1kΩ 1/4W | Base resistor (limits GPIO current) | 1 |
| LED1, LED2 | IR 940nm | Infrared emitter LEDs | 2 |

**Power Source:** USB 5V (from D1 Mini or separate supply)

## How the Transistor Works

```
    NPN Transistor (2N2222)
    Pin Assignment:
    
        C (Collector) ← Current flows IN from LEDs
         \
          |
    B ----+  (Base) ← Control signal from GPIO via R3
          |
         /
        E (Emitter) ← Current flows OUT to GND
```

**Operating Principle:**
- The transistor is a current-controlled switch
- A small current into the Base (from GPIO) allows a large current to flow from Collector to Emitter
- With R3 = 1kΩ: Base current = (3.3V - 0.7V) / 1kΩ ≈ 2.6mA
- Transistor gain (hFE) ≈ 100, so it can switch up to 260mA (plenty for our ~150mA LED current)

## Calculations

### LED Current (per LED)
```
I_LED = (V_supply - V_LED) / R_limit
I_LED = (5V - 1.4V) / 47Ω
I_LED = 3.6V / 47Ω ≈ 77mA
```

### Total Current Through Transistor
```
I_total = 2 × 77mA = 154mA
```
This is well within the 2N2222's 600mA maximum rating.

### Resistor Power Dissipation
```
P = I² × R = (0.077A)² × 47Ω ≈ 0.28W per resistor
```
Use 1/4W resistors (they'll run warm but within spec).

## PCB Layout Recommendations

1. **Keep traces short** between transistor and LEDs
2. **Add 0.1µF bypass capacitor** near transistor power pin if experiencing noise
3. **Mount LEDs on flexible leads** or IR lens holder for aiming adjustment
4. **Use shielded enclosure** to minimize EMI interference with meter reading
5. **Star-ground configuration** for analog reference (GND)

## Assembly Notes

- **Polarity**: IR LEDs have longer lead = Anode (positive)
- **Transistor orientation**: Flat side faces away from power
- **Base connection**: Connect D1 GPIO through 10kΩ pull-up to transistor base
- **Power source**: USB 5V supply (separate from ESP8266 if possible to avoid noise)

## Testing

1. Apply USB 5V power
2. Connect D1 GPIO5 to transistor base
3. Observe IR LEDs via camera/phone (will appear as purple/white light in camera)
4. Firmware outputs continuous mirror data on GPIO5 when meter transmits
5. Adjust LED aim toward meter's IR receiver window

## Software Integration

The firmware implements a `SoftwareSerial` instance on D1 (TX-only, inverted output) to drive this circuit:

```cpp
SoftwareSerial mirror(D5, D1, true);  // RX=D5(unused), TX=D1(GPIO5), inverted
```

All bytes received from the meter's SML protocol are immediately mirrored to this pin.
