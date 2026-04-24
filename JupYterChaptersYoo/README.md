# Dogbot Interactive Python Course (JupYterChapters)

Welcome to the interactive learning environment for programming your Dogbots! This folder contains a series of Jupyter Notebook chapters designed to take learners from basic Python programming all the way to controlling physical hardware.

## 🎯 Course Plan & Chapters

This course is structured progressively. Early chapters focus on teaching fundamental Python concepts using the interactive capabilities of Jupyter Notebooks. Once the basics are mastered, the course shifts towards physical computing—allowing learners to run Python cells that directly interact with the ESP32-C3 Dogbot over the network!

### 📚 Phase 1: Python Basics
- **Chapter 1: Hello World & Variables** - Understanding data types and basic output.
- **Chapter 2: Control Flow** - Using `if`, `else`, `for`, and `while` loops.
- **Chapter 3: Functions & Modules** - Organizing code and reusing logic.
- **Chapter 4: Data Structures** - Working with Lists, Dictionaries, and JSON.

### 🤖 Phase 2: Hardware Interaction (Dogbot API)
These later chapters utilize Python libraries (like `requests` or `websockets`) to send commands over Wi-Fi directly to the Dogbot's local web server (mDNS: `dogbot<ID>.local`).

- **Chapter 5: Remote Control & Servos** - Sending HTTP commands to actuate the 4 leg servos and choreograph movements.
- **Chapter 6: Visuals & OLED Display** - Pushing custom graphics, eye animations, and text to the robot's SPI display.
- **Chapter 7: Making Noise (Audio Output)** - Streaming PCM audio and utilizing the PDM speaker to make the robot bark or speak.
- **Chapter 8: Sensors & Mic Input** - Fetching sensor telemetry (wake button states) and processing microphone data.

## 🛠️ Fleet Management & Flashing
If you are an administrator preparing a fleet of Dogbots for a classroom, use the **`FlasherNotebook.ipynb`** included in this folder. It helps automate the process of building the firmware with unique device IDs (`-DDEVICE_NUMBER=X`) and tracking which robots have been successfully flashed and deployed.

Enjoy learning and building with your Dogbots!
