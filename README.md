# Terminal-UPS-Guard-
High-Performance TUI UPS Monitoring Daemon for Raspberry Pi 5 with Waveshare UPS HAT (E)

A **lightweight, high-performance UPS monitoring system** built entirely in C/C++ with ncurses-based TUI. Unlike GUI alternatives, this solution provides **instant emergency notifications** directly to all active terminals with **minimal resource footprint** - perfect for headless servers and critical infrastructure.

## 🚀 **Performance Comparison**

> **Measured on Raspberry Pi 5 with 4GB RAM, Kali Linux 2024.1**

| Feature | This Project (C/C++ TUI) | Python/Qt5 GUI | Advantage |
|---------|-------------------------|----------------|-----------|
| **Memory Usage (RES)** | **2.5 MB** | **14.6 MB** | **84% less memory** |
| **Memory Usage (VIRT)** | **14.4 MB** | **96.3 MB** | **85% less virtual memory** |
| **CPU Usage (Idle)** | **0.0%** | **0.5%** | **Zero idle CPU** |
| **Terminal Support** | All terminals (SSH, serial, local) | Requires X11/Wayland | **True headless** |

## ✨ **Core Features**

### 🏎️ **Performance First**
- **C/C++ Implementation** - Direct hardware access, no interpreters
- **Zero GUI Dependencies** - Runs on pure terminal, perfect for servers
- **Minimal Resource Footprint** - Uses <5MB RAM, <1% CPU
- **Instant Response** - Sub-second event detection and notification

### 🖥️ **Advanced TUI Interface**
- **ncurses-based Popups** - Professional terminal notifications
- **Multi-terminal Broadcasting** - Alerts on ALL active terminals
- **Color-coded Warnings** - Red/Yellow/Green based on severity
- **Real-time Countdown** - Visual shutdown timer during emergencies

### 🔋 **Complete UPS Monitoring**
- **Direct I2C Register Access** - No middleware, pure hardware communication
- **Three Event Types**:
  - 🔴 **Critical Battery** - Immediate shutdown warning
  - 🟡 **Mains Power Lost** - UPS switched to battery
  - 🟢 **Mains Restored** - Power restoration confirmation
- **Configurable Thresholds** - Custom voltage limits and timing

### ⚙️ **Production Ready**
- **systemd Service Integration** - Auto-start, logging, monitoring
- **Signal Handling** - Graceful termination on shutdown
- **Terminal Resize Support** - Adapts to any terminal size
- **Non-blocking Architecture** - Monitoring continues during popups


## 🛠️ **Supported Hardware & Software**

### **Hardware**
- **SBC**: Raspberry Pi 5 (all variants)
- **UPS**: Waveshare UPS HAT (E) - I2C interface, 2-4 cell Li-ion
- **Compatible**: Any system with I2C and Linux


### **Step-by-Step Manual Installation**
#### **1. Install Dependencies**
```bash
# Kali / Debian / Ubuntu
sudo apt update
sudo apt install -y gcc g++ libncurses-dev i2c-tools

#### **2. Compile the Programs**
```bash
# Compile the main monitoring daemon
g++ -o ups_monitor_daemon ./ups_monitor_daemon.cpp
```

# Compile the TUI popup interface
gcc -o ups_tui ./ups_tui.c -lncurses

#### **3.  Install to System Directories**
```bash
# Copy executables to /usr/local/bin
sudo cp ups_monitor_daemon ups_tui /usr/local/bin/
```

#### **4.  Configure Systemd Service**
```bash
# Copy the systemd service file
sudo cp ups-monitor.service /etc/systemd/system/
```

#### **5. Reload and Enable Service**
```bash
sudo systemctl daemon-reload

# Enable service to start on boot
sudo systemctl enable ups-monitor

# Start the service immediately
sudo systemctl start ups-monitor

# Check service status
sudo systemctl status ups-monitor
```
