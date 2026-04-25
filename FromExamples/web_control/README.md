# Servo Dog Web Control Example

Web-based servo dog control example. You can remotely control the servo dog's movement and perform servo calibration via the web interface.

## Features

- Web control interface, supports mobile access
- Virtual joystick control, supports forward, backward, left, and right movement
- Preset action control
- Servo calibration function
- Supports WiFi connection and SoftAP mode
- Supports mDNS, accessible via hostname

## Configuration

Before compiling, you can configure the following using `idf.py menuconfig`:

### WiFi Settings

- WiFi SSID: Name of the WiFi network to connect to
- WiFi Password: WiFi password
- SoftAP SSID: Name of the WiFi hotspot created by ESP32 (default is "ESP-Hi")
- SoftAP Password: Hotspot password
- SoftAP Channel: Hotspot channel (default is 1)
- SoftAP IP: Hotspot IP address (default is 192.168.4.1)
- MDNS Hostname: mDNS hostname (default is "esp-hi", access via `http://esp-hi.local/`)

## Usage Instructions

1. Compile and flash the program to ESP32
2. Based on the configured WiFi mode:
   - If connected via WiFi, the ESP32 will connect to the specified WiFi network
   - If configured as SoftAP, the ESP32 will create a WiFi hotspot
3. Access the control interface:
   - If using mDNS, visit `http://[MDNS Hostname].local`
   - If using SoftAP, visit `http://192.168.4.1`
   - If using WiFi connection, visit the ESP32's IP address

### Control Interface

- **Control Page**:
  - Use the virtual joystick to control the servo dog's movement
  - Use preset action buttons to execute specific actions

- **Servo Calibration Page**:
  - Please follow the calibration tutorial prompts on the servo calibration page

## Precautions

1. Servo calibration must be performed before the first use
2. Do not manually bend the servos with your hands to avoid damage
