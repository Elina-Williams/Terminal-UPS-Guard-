
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <array>
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <dirent.h>
#include <pwd.h>
#include <libgen.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <iomanip>
#include <regex>
// For Socket Communication
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <poll.h>

/*
* ===== Key Design Features =====
* Non-blocking architecture: Main monitoring continues while popups display
* Graceful cancellation: Immediate popup termination when conditions change
* User-centric display: Shows notifications on ALL active user terminals
* Configurable timing: Different display durations for different event types
* Systemd-friendly: Can run as background service with TUI popups to user sessions
*/

// Reference: https://www.waveshare.com/wiki/UPS_HAT_(E)_Register

/*
# ===== Overall Architecture =====

# UPS Monitoring Daemon (Background)
# ├── Main Monitoring Loop
# ├── Popup Queue Manager (PopupQueue)
# └── TUI Popup Display (ups_tui.py on active terminals)

# =====  Data Collection & Monitoring Loop =====

# Continuously reads battery data via I2C (SMBus) every CHECK_INTERVA_* s
# Detects three critical events:
#   1. Low battery voltage (<LOW_VOLTAGE mV per cell) when discharging
#   2. UPS switched to battery power (mains power lost)
#   3. Mains power restored

# Power State Machine:
# [AC Power] → (AC Lost) → [On Battery] → (Low Voltage) → [Shutdown Countdown]
#       ↑                                         |
#       └─────────────────────────────────────────┘
#             (AC Restored - Cancels shutdown)
*/


// I2C Device class
/* Use "ls -l /dev/i2c*" to view all I2C device files
   e.g. 
   crw-rw---- 1 root i2c 89,  1 Dec 14 10:22 /dev/i2c-1
   crw-rw---- 1 root i2c 89, 13 Dec 14 10:22 /dev/i2c-13

  I2C bus number is typically 1
*/
class I2CDevice {
private:
    int file;
    int address;

public:
    I2CDevice(int bus, int addr) : address(addr) {
        std::string deviceName = "/dev/i2c-" + std::to_string(bus);
        file = open(deviceName.c_str(), O_RDWR);
        if (file < 0) {
            throw std::runtime_error("Failed to open I2C device");
        }

        // Set slave device address
        if (ioctl(file, I2C_SLAVE, address) < 0) {
            close(file);
            throw std::runtime_error("Failed to set I2C address");
        }
    }

    ~I2CDevice() {
        if (file >= 0) {
            close(file);
        }
    }
    
    /*
    Most I2C devices have multiple internal registers. Each register has a unique address 
    (usually 1-2 bytes) and store specific data or controls particular functions
    
    Standard I2C Read Operation:
    ┌─────┬────────────┬──────┬─────────────┬─────────┬────────────┬────────┐
    │START│ Slave Addr │ Write│ Reg Address │ RESTART │ Slave Addr │ Read   │
    │     │  (7-bit)   │ Bit  │  (e.g.0x03) │         │  (7-bit)   │ Data   │
    │     │            │ (0)  │             │         │     +      │        │
    │     │            │      │             │         │  Read (1)  │        │
    │     │            │      │             │         │            │        │
    └─────┴────────────┴──────┴─────────────┴─────────┴────────────┴────────┘
    */

    std::vector<uint8_t> readBlock(uint8_t reg, uint8_t length) {
        std::vector<uint8_t> buffer(length);

        struct i2c_rdwr_ioctl_data packets;
        struct i2c_msg messages[2];

        // Write the register address to the device
        uint8_t reg_addr = reg;
        messages[0].addr = address;
        messages[0].flags = 0;  // 写操作
        messages[0].len = 1;
        messages[0].buf = &reg_addr;

        // read the value FROM the register 
        messages[1].addr = address;
        messages[1].flags = I2C_M_RD;  // 读操作
        messages[1].len = length;
        messages[1].buf = buffer.data();
        
        packets.msgs = messages;
        packets.nmsgs = 2;

        if (ioctl(file, I2C_RDWR, &packets) < 0) {
            throw std::system_error(errno, std::system_category(),
                                   "Failed to perform I2C block read");
        }
        return buffer;
    }

    void writeByte(uint8_t reg, uint8_t value) {
        uint8_t buffer[2] = {reg, value};
        if (write(file, buffer, sizeof(buffer)) != sizeof(buffer)) {
            throw std::runtime_error("Failed to write byte");
        }
    }
};

struct Config {
    int LOW_VOLTAGE = 3150;           // mV
    int CHECK_INTERVAL_NORMAL = 60;    // seconds
    int CHECK_INTERVAL_LOW = 10;       // seconds
    int SHUTDOWN_TIMEOUT = 30;        // seconds
    int ALERT_TIMEOUT = 5;            // seconds
    std::string POPUP_EXECUTABLE;

    Config() {
        char exePath[1024];
        // Get path to the compiled C TUI executable
        ssize_t count = readlink("/proc/self/exe", exePath, sizeof(exePath)-1);
        /* the symbolic link /proc/self/exe, which points to
        the absolute path of the current program's executable file.
        -1 leaves space for '\0' */
        if (count != -1) {
            exePath[count] = '\0';
            std::string dir = dirname(exePath);
            POPUP_EXECUTABLE = dir + "/ups_tui";
        } else {
            POPUP_EXECUTABLE = "/usr/local/bin/ups_tui";
        }
        // count is the number of characters read, and -1 indicates an error.
    }
};

// ===== Start declare data structure =====

// Battery data Structure
struct BatteryData {
    uint8_t status;
    float battery_voltage;
    float battery_current;
    int battery_percent;
    int battery_capacity;
    int time_to_empty;
    int time_to_full;
    std::vector<int> cell_voltages;
    float vbus_voltage;
    float vbus_current;
    float vbus_power;
};

// Active terminal information
struct TerminalInfo {
    std::string user;
    std::string tty;
    std::string full_path;
};

// Popup request structure
struct PopupRequest {
    int battery_percent;
    int msg_type;
    int count_timer;
};


/* =====
Popup Queue Processing
    1. Continuously attempt to fetch tasks from queue (1s timeout)
    2. Upon receiving task → Display popup on ALL active terminals
    3. Begin processing cycle:
       - Shutdown warnings (Type 1): SHUTDOWN_TIMEOUT-second display
       - Notifications (Types 2,3): ALERT_TIMEOUT-second display
    4. During processing:
       - Monitor for cancellation events (via `cancel_current_popup()`)
       - If cancelled → Immediately terminate all popup processes
    5. After completion (natural or cancelled):
       - Reset state
       - Fetch next task from queue
*/
class PopupQueue {
private:
    std::queue<PopupRequest> requestQueue;
    std::mutex queueMutex;
    std::condition_variable queueCV;
    std::vector<pid_t> currentPids;
    std::mutex pidsMutex;
    std::atomic<bool> running{true};
    std::thread workerThread;
    Config config;

    // Helper function for 'getActiveTerminals'
    std::string exec_command(const char* cmd) {
        std::array<char, 256> buffer;
        std::string result;

        auto deleter = [](FILE* f) { 
            if (f) pclose(f); 
        };
    
        std::unique_ptr<FILE, decltype(deleter)> pipe(popen(cmd, "r"), deleter);
        if (!pipe) {
            throw std::runtime_error("popen() failed!");
        }
        while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
            result += buffer.data();
        }
        return result;
    }
    
    // Get all active terminals on Debian
    std::vector<TerminalInfo> getActiveTerminals() {
        std::vector<TerminalInfo> active_terminals;

        try {
            std::string output = exec_command("who");

            std::istringstream stream(output);
            std::string line;
            std::regex pattern(R"(^(\w+)\s+.*?(tty\w*|pts/\d+))");

            while (std::getline(stream, line)) {
                std::smatch match;
                if (std::regex_search(line, match, pattern) && match.size() >= 3) {
                    TerminalInfo info;
                    info.user = match[1].str();
                    info.tty = match[2].str();
                    info.full_path = "/dev/" + info.tty;
                    active_terminals.push_back(info);
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << std::endl;
        }
        return active_terminals;
    }

    // Spawns a child process to display a popup message on a specific terminal.
    pid_t showOnTerminal(const TerminalInfo& terminal, int battery_percent, int msg_type, int count_timer)
    {
        // Fork to create child process
        pid_t pid = fork();

        if (pid == -1) {
            // Fork failed - log error and return failure
            perror("fork() failed");
            return -1;
        }
    
        if (pid == 0) {
            // --- CHILD PROCESS ---
            // Create new session to detach from parent terminal
            if (setsid() == -1) {
                perror("setsid() failed");
                _exit(EXIT_FAILURE);  // Use _exit() in child after fork
            }
            // Open terminal device for writing
            int tty_fd = open(terminal.full_path.c_str(), O_WRONLY | O_CLOEXEC);
            if (tty_fd < 0) {
                perror("Failed to open terminal device");
                _exit(EXIT_FAILURE);
            }

            // Redirect standard output and error to terminal
            if (dup2(tty_fd, STDOUT_FILENO) == -1 || dup2(tty_fd, STDERR_FILENO) == -1) 
            {
                perror("dup2() failed");
                close(tty_fd);
                _exit(EXIT_FAILURE);
            }

            // Close the original file descriptor (dup2 created copies)
            close(tty_fd);
        
            // Close standard input
            close(STDIN_FILENO);
        
            // Prepare command arguments
            std::string battery_str = std::to_string(battery_percent);
            std::string type_str = std::to_string(msg_type);
            std::string timer_str = std::to_string(count_timer);

            // Execute the popup program with arguments
            execl(config.POPUP_EXECUTABLE.c_str(),
                  config.POPUP_EXECUTABLE.c_str(),
                  "--type", type_str.c_str(),
                  "--battery", battery_str.c_str(),
                  "--timer", timer_str.c_str(),
                  nullptr);
        
            // exec only returns on error
            perror("execl() failed");
            _exit(EXIT_FAILURE);
        }
        return pid;
    }


    // 
    void waitForPopupCompletion(const std::vector<pid_t>& pids, int timeout) {
        time_t startTime = time(nullptr);
        std::vector<pid_t> remainingPids = pids;

        while (time(nullptr) - startTime < timeout && !remainingPids.empty())
        {
            for (auto it = remainingPids.begin(); it != remainingPids.end(); ) {
                int status;
                pid_t result = waitpid(*it, &status, WNOHANG);
                
                if (result == *it || result == -1) {
                    // Process has finished or error
                    it = remainingPids.erase(it);
                } else {
                    ++it;
                }
            }
            
            if (remainingPids.empty()) {
                break;
            }
            
            usleep(500000); // Sleep 500ms
        }
        // Kill any remaining processes
        for (pid_t pid : remainingPids) {
            killpg(pid, SIGTERM);
        }
    }

    // Key function in this class
    void processQueue() {
        while(running) {
            PopupRequest request;
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                if (queueCV.wait_for(lock, std::chrono::seconds(1), 
                    [this] { return !requestQueue.empty() || !running; }))
                {
                    // [this] { return !requestQueue.empty() || !running; }
                    // 谓词条件. 这两个条件满足任意一个，等待就结束
                    if (!running) break;
                    
                    request = requestQueue.front();
                    requestQueue.pop();
                } else {
                    continue;
                }
            }
            // Show popup
            showSinglePopup(request.battery_percent, request.msg_type, request.count_timer);      
        }
    }


    //
    void showSinglePopup(int battery_percent, int msg_type, int count_timer) 
    {
        std::vector<TerminalInfo> terminals = getActiveTerminals();

        if (terminals.empty()) {
            std::cerr << "Warning: No active terminals found" << std::endl;
            return;
        }

        // Display popups on all active terminals
        std::vector<pid_t> pids;
        for (const auto& terminal : terminals) {
            pid_t pid = showOnTerminal(terminal, battery_percent, msg_type, count_timer);
            if (pid > 0) pids.push_back(pid);
        }

        {
            std::lock_guard<std::mutex> lock(pidsMutex);
            currentPids = pids;
        }

        waitForPopupCompletion(pids, count_timer);

        {
            std::lock_guard<std::mutex> lock(pidsMutex);
            currentPids.clear();
        }
    }


public:
    PopupQueue() { workerThread = std::thread(&PopupQueue::processQueue, this); }

    ~PopupQueue() {
        stop();
        if (workerThread.joinable()) {
            workerThread.join();
        }
    }

    void stop() {
        running = false;
        queueCV.notify_all();
        cancelCurrentPopup();
    }

    void cancelCurrentPopup() {
        std::lock_guard<std::mutex> lock(pidsMutex);
        for (pid_t pid : currentPids) {
            killpg(pid, SIGTERM);
        }
        currentPids.clear();
    }

    void addPopup(int battery_percent, int msg_type, int count_timer)
    {
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            requestQueue.push({battery_percent, msg_type, count_timer});
        }
        queueCV.notify_one();
    }
};

// ===== Battery Monitor Class =====
class BatteryMonitor {
private:
    I2CDevice i2c;
    Config config;
    PopupQueue popupQueue;

    std::atomic<int> shutdownCounter{0};
    std::atomic<bool> shutdownActive{false};
    std::atomic<bool> firstRun{true};
    std::atomic<bool> isDischarging{false};
    
    std::thread shutdownThread;
    std::atomic<bool> shutdownThreadRunning{false};
    std::condition_variable shutdownCV;
    std::mutex shutdownMutex;

    // For Socket Communication
    /** @brief Thread that runs the socket server loop. */
    std::thread socketThread;
    /** @brief Atomic flag to gracefully terminate the socket thread. */
    std::atomic<bool> socketRunning{false};
    /** @brief Mutex to protect `lastJsonData` from concurrent access.
     *         Held when updating (by main I2C loop) or reading (by socket thread). 
     */
    std::mutex dataMutex;
    std::string lastJsonData;

    static const int I2C_ADDR = 0x2d;
    static const std::string SOCKET_PATH;   // 在类外定义

    /**
     * @brief Serializes a BatteryData structure into a compact JSON string.
     * @param data  The battery data to convert.
     * @return std::string  JSON object with keys: voltage, current, percent,
     *                      capacity, time_to_empty, time_to_full, vbus_voltage,
     *                      vbus_current, vbus_power, status.
     */
    std::string buildJson(const BatteryData& data);

    /**
     * @brief Main loop for the socket server thread.
     * 
     * Creates a listening Unix socket at SOCKET_PATH, accepts incoming connections,
     * and sends the latest JSON data (protected by dataMutex) to each client.
     * The socket is set to non‑blocking mode to allow periodic exit checks.
     */
    void socketServerLoop();

public:
    BatteryMonitor() : i2c(1, I2C_ADDR) {
        {
            std::lock_guard<std::mutex> lock(dataMutex);
            lastJsonData = "{\"error\":\"no data yet\"}";
        }
        socketThread = std::thread(&BatteryMonitor::socketServerLoop, this);
    }

    BatteryData readBatteryData() {
        BatteryData data;

        // Read status
        auto statusData = i2c.readBlock(0x02, 0x01);
        data.status = statusData[0];

        // Read VBUS Data 
        auto vbusData = i2c.readBlock(0x10, 0x06);
        data.vbus_voltage = static_cast<float>((vbusData[0] | (vbusData[1] << 8))) / 1000.0f;
        data.vbus_current = static_cast<float>((vbusData[2] | (vbusData[3] << 8))) / 1000.0f;
        data.vbus_power = static_cast<float>((vbusData[4] | (vbusData[5] << 8))) / 1000.0f;

        // Read main Battery data
        auto batteryData = i2c.readBlock(0x20, 0x0C);
        data.battery_voltage = static_cast<float>((batteryData[0] | (batteryData[1] << 8))) / 1000.0f;
        // battery current (signed)
        int16_t battery_current_raw = batteryData[2] | (batteryData[3] << 8);
        if (battery_current_raw > 0x7FFF) battery_current_raw -= 0x7FFF;
        data.battery_current = static_cast<float>(battery_current_raw) / 1000.0f;

        data.battery_percent = batteryData[4] | (batteryData[5] << 8);
        data.battery_capacity = batteryData[6] | (batteryData[7] << 8);
        data.time_to_empty = batteryData[8] | (batteryData[9] << 8);
        data.time_to_full = batteryData[10] | (batteryData[11] << 8);

        // Read Cell voltages
        auto cellData = i2c.readBlock(0x30, 0x08);
        data.cell_voltages.resize(4);
        for (int i = 0; i < 4; i++) {
            data.cell_voltages[i] = cellData[i*2] | (cellData[i*2 + 1] << 8);
        }

        return data;
    }


    void checkAndNotifyPowerChanges(const BatteryData& data) 
    {
        // Upper 7th bit setted represents fast-charging
        // Upper 8th bit setted represents charging 
        bool currentCharging = (data.status & 0x40) || (data.status & 0x80);
        bool currentDischargingOrIdle = !currentCharging;

        // Check if started discharging (mains lost)
        if (currentDischargingOrIdle && !isDischarging) {
            popupQueue.addPopup(data.battery_percent, 2, config.ALERT_TIMEOUT);
            isDischarging = true;
        }
        // Check mains restored (from discharging to charging)
        else if (isDischarging && currentCharging) {
            // If shutdown is active, cancel it
            if (shutdownActive) {
                {
                    std::lock_guard<std::mutex> lock(shutdownMutex);
                    shutdownActive = false;
                    shutdownCounter = 0;
                }
                shutdownCV.notify_all();
            
                popupQueue.cancelCurrentPopup();

                popupQueue.addPopup(data.battery_percent, 3, config.ALERT_TIMEOUT);
            } else {
                popupQueue.addPopup(data.battery_percent, 3, config.ALERT_TIMEOUT);
            }

            isDischarging = false;
        }
    }


    bool checkLowVoltage(const std::vector<int>& cell_voltages, bool charging, int battery_percent) 
    {
        bool low_voltage_detected = false;

        for (int voltage : cell_voltages) {
            if (voltage < config.LOW_VOLTAGE) {
                low_voltage_detected = true;
                break;
            }
        }

        // If low voltage detected and not charging
        if (low_voltage_detected && !charging) 
        {
            if(!shutdownActive) {
                shutdownCounter = config.SHUTDOWN_TIMEOUT;
                shutdownActive = true;

                // Add to queue
                popupQueue.addPopup(battery_percent, 1, config.SHUTDOWN_TIMEOUT);

                // Start shutdown countdown thread
                if (shutdownThreadRunning) {
                    shutdownThread.join();
                }
                
                shutdownThreadRunning = true;
                shutdownThread = std::thread(&BatteryMonitor::shutdownCountdown, this);
            }
        }

        return low_voltage_detected;
    }


    void shutdownCountdown() 
    {
        int remaining_time = shutdownCounter;

        while(remaining_time > 0 && shutdownActive)
        {
            std::unique_lock<std::mutex> lock(shutdownMutex);
            if (shutdownCV.wait_for(lock, std::chrono::seconds(1), 
                [this] { return !shutdownActive; })) {
                break;
            }
            
            remaining_time--;
            shutdownCounter = remaining_time;
        }

        // If countdown completed naturally (not interrupted), initiate shutdown
        if (remaining_time <= 0 && shutdownActive) {
            initiateShutdown();
        }

        shutdownThreadRunning = false;
    }


    void initiateShutdown() {
        // Check if device is still online
        try {
            FILE* fp = popen("i2cdetect -y -r 1 0x2d 0x2d | grep -E '2d' | awk '{print $2}'", "r");

            if (fp) {
                char buffer[16];
                if (fgets(buffer, sizeof(buffer), fp))
                {
                    buffer[strcspn(buffer, "\n")] = 0;
                    if (strcmp(buffer, "2d") == 0) {
                        // Device online, send shutdown command
                        // Write 0x55 to register 0x01
                        system("i2cset -y 1 0x2d 0x01 0x55");
                    }
                }
                pclose(fp);
            }
        } catch (...) {
            std::cerr << "Error checking device" << std::endl;
        }

        // Execute shutdown
        system("sudo poweroff");
    }


    void checkOnece() {
        try {
            BatteryData data = readBatteryData();

            // ===== New: Update shared data for socket communication =====
            {
                std::lock_guard<std::mutex> lock(dataMutex);
                lastJsonData = buildJson(data);
            }

            // Check power status changes and notify
            if (firstRun) {
                // Initialise state
                isDischarging = !((data.status & 0x40) || (data.status & 0x80));
                firstRun = false;
                sleep(5);
                return;
            }

            checkAndNotifyPowerChanges(data);

            // Check low voltage and handle shutdown
            bool charging = (data.status & 0x40) || (data.status & 0x80);
            bool low_voltage_detected = checkLowVoltage(data.cell_voltages, charging, data.battery_percent);
                    
            if (low_voltage_detected) {
                sleep(config.CHECK_INTERVAL_LOW);
            } else {
                sleep(config.CHECK_INTERVAL_NORMAL);
            }
                
        } catch (const std::exception& e) {
            std::cerr << "Error reading battery data: " << e.what() << std::endl;
            sleep(50);
        }
    }

    void run()
    {
        try {
            while(true)
            {
                checkOnece();
            }
        } catch (const std::exception& e) {
            std::cerr << "Program runtime error: " << e.what() << std::endl;
        }
    }

    void stop() {
        shutdownActive = false;
        shutdownCV.notify_all();

        if (shutdownThread.joinable()) {
            shutdownThread.join();
        }

        // Stop socket server
        socketRunning = false;
        if (socketThread.joinable()) {
            socketThread.join();
        }

        popupQueue.stop();
    }
};

// The socket file is placed in the /tmp directory (tmpfs memory filesystem) 
// to avoid frequent writes to the SD card and to ensure automatic cleanup on reboot.
const std::string BatteryMonitor::SOCKET_PATH = "/tmp/ups.sock";

std::string BatteryMonitor::buildJson(const BatteryData &data) {
    std::ostringstream oss;
    oss << "{"
        << "\"voltage\":" << data.battery_voltage << ","
        << "\"current\":" << data.battery_current << ","
        << "\"percent\":" << data.battery_percent << ","
        << "\"capacity\":" << data.battery_capacity << ","
        << "\"time_to_empty\":" << data.time_to_empty << ","
        << "\"time_to_full\":" << data.time_to_full << ","
        << "\"vbus_voltage\":" << data.vbus_voltage << ","
        << "\"vbus_current\":" << data.vbus_current << ","
        << "\"vbus_power\":" << data.vbus_power << ","
        << "\"status\":" << static_cast<int>(data.status)
        << "}";
    return oss.str();    
}

void BatteryMonitor::socketServerLoop() {
    int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return;
    }
    // Delete existing socket file if it exists
    unlink(SOCKET_PATH.c_str());

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH.c_str(), sizeof(addr.sun_path) - 1);
    addr.sun_path[sizeof(addr.sun_path) - 1] = '\0';

    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(listen_fd);
        return;
    }

    // Set permissions to allow read/write for all users
    // Note that the actual permission is `mode & ~umask`, so we temporarily set umask to 0 
    // to ensure the desired permissions are applied.
    mode_t old_mask = umask(0);
    chmod(SOCKET_PATH.c_str(), 0666);
    umask(old_mask);

    if (listen(listen_fd, 5) < 0) {
        perror("listen");
        close(listen_fd);
        return;
    }

    socketRunning = true;
    // Loop to detect new connections. If none, sleep 100ms and continue, until socketRunning is set to false.
    while (socketRunning) {
        struct pollfd pfd;
        pfd.fd = listen_fd;
        pfd.events = POLLIN;
        
        int timeout = 100; 
        int ret = poll(&pfd, 1, timeout);

        if (ret < 0) {
            if (errno == EINTR) {
                continue; // Interrupted by signal, retry
            }
            if (errno == EBADF) {
                // Socket has been closed, exit the loop
                break;
            }
            perror("poll");
            break;
        } else if (ret == 0) {
            // Timeout, no incoming connection, continue to next iteration
            continue;
        }
     
        // ret > 0, there is an incoming connection
        if (pfd.revents & POLLIN) { 
            int client_fd = accept(listen_fd, nullptr, nullptr);
            if (client_fd < 0) {
                // if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                    continue;
                }
                if (errno == ECONNABORTED) {
                    continue; 
                    // Client closed the connection before accept() could complete.
                }
                perror("accept");
                break;
            }

            std::string json_to_send;
            // Read the latest JSON data (protected by dataMutex) and send to client when a connection is established
            {
                std::lock_guard<std::mutex> lock(dataMutex);
                json_to_send = lastJsonData.empty() ? "{\"error\":\"no data yet\"}" : lastJsonData;
            }
            /**
            * If the client closed the connection before or during the server's send call, 
            * send will attempt to write to a "closed pipe", which will result in a SIGPIPE signal 
            * being sent to the process. To prevent the process from being terminated by this signal, 
            * we can use the MSG_NOSIGNAL flag in the send() call. 
            * This flag tells the kernel not to send a SIGPIPE signal if the other end has closed the connection.
            */
            const char* ptr = json_to_send.c_str();
            size_t total = json_to_send.size();
            while (total > 0) {
                ssize_t n = send(client_fd, ptr, total, MSG_NOSIGNAL);
                if (n <= 0) {
                    if (errno == EINTR) continue;
                    break; // Failed to send, exit the loop
                }
                ptr += n;
                total -= n;
            }
            // ! Consider the partial send case here. Use a loop to ensure all data is sent. 
            close(client_fd);
        }
        // Every time a new connection is established, the latest JSON data is sent to the client, 
        // and then the connection is closed.
    }

    close(listen_fd);
    unlink(SOCKET_PATH.c_str());   // Clean up socket file on exit
}

// Signal handler
volatile bool keepRunning = true;

void signalHandler(int signal) {
    keepRunning = false;
}

int main(int argc, char* argv[]) {
    // Set up signal handlers
    signal(SIGINT,  signalHandler); // Ctrl + C 
    signal(SIGTERM, signalHandler); // Kill 

    try {
        auto monitor = std::make_unique<BatteryMonitor>();
        
        while(keepRunning) {
            monitor->checkOnece();
        }

        monitor->stop();

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    } 
    return 0;
}