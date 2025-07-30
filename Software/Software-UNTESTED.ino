#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <SPIFFS.h>

// ============================================================================
// CONFIGURATION AND CONSTANTS
// ============================================================================

// WiFi Configuration
const char* WIFI_SSID = "your_wifi_ssid";
const char* WIFI_PASSWORD = "your_wifi_password";

// Hardware I2C Addresses
#define TCA9548_ADDRESS     0x70    // I2C multiplexer
#define BQ25895_ADDRESS     0x6A    // Battery charging IC (same on all channels)

// I2C Pin Configuration for ESP32-C6
#define I2C_SDA_PIN         6
#define I2C_SCL_PIN         7

// Charging System Configuration
#define NUM_CHARGERS        3
#define CHARGER_CHANNELS    {0, 1, 2}  // TCA9548 channels for each charger
#define TARGET_CHARGE_CURRENT_MA  500  // 0.5A charging current

// Web Server
AsyncWebServer server(80);

// ============================================================================
// DATA STRUCTURES
// ============================================================================

struct BatteryData {
    uint8_t channel;
    float voltage_v;          // Battery voltage in volts
    float charge_current_ma;  // Charging current in mA
    float bus_voltage_v;      // Input bus voltage
    uint8_t charge_status;    // Charging state (0=not charging, 1=pre-charge, 2=fast charge, 3=done)
    uint8_t fault_status;     // Fault register value
    bool is_charging;         // Simplified charging indicator
    bool has_fault;          // Simplified fault indicator
    uint32_t last_update;    // Timestamp of last update
    bool is_online;          // Device communication status
};

struct SystemStatus {
    uint32_t uptime_ms;
    uint8_t active_chargers;
    int8_t wifi_rssi;
    uint32_t free_heap;
    bool system_healthy;
};

// ============================================================================
// GLOBAL VARIABLES
// ============================================================================

BatteryData batteries[NUM_CHARGERS];
SystemStatus system_status;
uint8_t charger_channels[] = CHARGER_CHANNELS;
SemaphoreHandle_t i2c_mutex;

// ============================================================================
// BQ25895RTW REGISTER DEFINITIONS AND FUNCTIONS
// ============================================================================

// BQ25895RTW Register Addresses
#define BQ25895_REG_ILIM         0x00  // Input current limit
#define BQ25895_REG_VINDPM       0x01  // Input voltage limit
#define BQ25895_REG_ADC          0x02  // ADC control
#define BQ25895_REG_SYS          0x03  // System configuration
#define BQ25895_REG_ICHG         0x04  // Fast charge current
#define BQ25895_REG_IPRECHG      0x05  // Pre-charge/termination current
#define BQ25895_REG_VREG         0x06  // Charge voltage limit
#define BQ25895_REG_TIMER        0x07  // Charge timer control
#define BQ25895_REG_BAT_COMP     0x08  // Battery compensation
#define BQ25895_REG_BOOST        0x0A  // Boost configuration
#define BQ25895_REG_STATUS       0x0B  // System status
#define BQ25895_REG_FAULT        0x0C  // Fault status
#define BQ25895_REG_VINDPM_STAT  0x0D  // Input voltage status
#define BQ25895_REG_ADC_VBUS     0x0E  // ADC Bus voltage
#define BQ25895_REG_ADC_VPMID    0x0F  // ADC PMID voltage
#define BQ25895_REG_ADC_VBAT     0x10  // ADC Battery voltage
#define BQ25895_REG_ADC_VSYS     0x11  // ADC System voltage  
#define BQ25895_REG_ADC_ICHG     0x12  // ADC Charge current
#define BQ25895_REG_ADC_IDPM     0x13  // ADC Input current
#define BQ25895_REG_RESET        0x14  // Device reset

// Configuration values for 0.5A (512mA actual) charging current
struct BQ25895Config {
    uint8_t reg;
    uint8_t value;
    const char* description;
};

const BQ25895Config bq25895_init_sequence[] = {
    {BQ25895_REG_RESET,    0x80, "Software reset"},
    {BQ25895_REG_ILIM,     0x30, "Input current limit ~500mA, disable ILIM pin"},
    {BQ25895_REG_ADC,      0x9D, "Enable continuous ADC conversion, ICO"},
    {BQ25895_REG_SYS,      0x1A, "Enable charging, 3.5V min system voltage"},
    {BQ25895_REG_ICHG,     0x08, "Fast charge current = 512mA (8 × 64mA)"},
    {BQ25895_REG_IPRECHG,  0x83, "Pre-charge 192mA, termination 192mA"},
    {BQ25895_REG_VREG,     0x5E, "Charge voltage 4.208V for LiPo"},
    {BQ25895_REG_TIMER,    0x8D, "Disable watchdog, enable charging safety timer"}
};

// ============================================================================
// I2C MULTIPLEXER FUNCTIONS
// ============================================================================

bool tca9548_select_channel(uint8_t channel) {
    if (channel > 7) return false;
    
    if (xSemaphoreTake(i2c_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        Serial.println("Failed to acquire I2C mutex");
        return false;
    }
    
    Wire.beginTransmission(TCA9548_ADDRESS);
    Wire.write(1 << channel);  // Enable only selected channel
    uint8_t error = Wire.endTransmission();
    
    if (error == 0) {
        delayMicroseconds(100);  // Channel settling time
        return true;
    } else {
        Serial.printf("TCA9548 channel %d selection failed: %d\n", channel, error);
        xSemaphoreGive(i2c_mutex);
        return false;
    }
}

void tca9548_release_channel() {
    // Deselect all channels
    Wire.beginTransmission(TCA9548_ADDRESS);
    Wire.write(0x00);
    Wire.endTransmission();
    
    xSemaphoreGive(i2c_mutex);
}

bool tca9548_scan_channels() {
    Serial.println("Scanning I2C channels...");
    bool found_devices = false;
    
    for (uint8_t channel = 0; channel < 8; channel++) {
        if (tca9548_select_channel(channel)) {
            Serial.printf("Channel %d: ", channel);
            bool found_on_channel = false;
            
            for (uint8_t addr = 1; addr < 127; addr++) {
                Wire.beginTransmission(addr);
                if (Wire.endTransmission() == 0) {
                    Serial.printf("0x%02X ", addr);
                    found_on_channel = true;
                    found_devices = true;
                }
            }
            
            if (!found_on_channel) {
                Serial.print("No devices");
            }
            Serial.println();
            
            tca9548_release_channel();
        }
    }
    
    return found_devices;
}

// ============================================================================
// BQ25895RTW COMMUNICATION FUNCTIONS
// ============================================================================

bool bq25895_write_register(uint8_t reg, uint8_t value) {
    Wire.beginTransmission(BQ25895_ADDRESS);
    Wire.write(reg);
    Wire.write(value);
    uint8_t error = Wire.endTransmission();
    
    if (error != 0) {
        Serial.printf("BQ25895 write reg 0x%02X failed: %d\n", reg, error);
        return false;
    }
    return true;
}

bool bq25895_read_register(uint8_t reg, uint8_t* value) {
    Wire.beginTransmission(BQ25895_ADDRESS);
    Wire.write(reg);
    uint8_t error = Wire.endTransmission(false);  // Repeated start
    
    if (error != 0) {
        Serial.printf("BQ25895 read setup reg 0x%02X failed: %d\n", reg, error);
        return false;
    }
    
    Wire.requestFrom(BQ25895_ADDRESS, (uint8_t)1);
    if (Wire.available()) {
        *value = Wire.read();
        return true;
    } else {
        Serial.printf("BQ25895 read reg 0x%02X no data\n", reg);
        return false;
    }
}

bool bq25895_initialize() {
    Serial.println("Initializing BQ25895RTW for 0.5A charging...");
    
    // Software reset first
    if (!bq25895_write_register(BQ25895_REG_RESET, 0x80)) {
        Serial.println("BQ25895 reset failed");
        return false;
    }
    delay(10);  // Wait for reset
    
    // Clear reset bit
    if (!bq25895_write_register(BQ25895_REG_RESET, 0x00)) {
        return false;
    }
    delay(10);
    
    // Apply configuration sequence
    const int config_count = sizeof(bq25895_init_sequence) / sizeof(bq25895_init_sequence[0]);
    
    for (int i = 1; i < config_count; i++) {  // Skip first entry (reset) 
        const BQ25895Config& config = bq25895_init_sequence[i];
        
        if (!bq25895_write_register(config.reg, config.value)) {
            Serial.printf("BQ25895 config failed at reg 0x%02X: %s\n", 
                         config.reg, config.description);
            return false;
        }
        delay(1);
    }
    
    Serial.println("BQ25895RTW initialization complete");
    return true;
}

bool bq25895_read_status(BatteryData* battery) {
    uint8_t status_reg, fault_reg, adc_vbat, adc_vbus, adc_ichg;
    
    // Read critical registers
    if (!bq25895_read_register(BQ25895_REG_STATUS, &status_reg) ||
        !bq25895_read_register(BQ25895_REG_FAULT, &fault_reg) ||
        !bq25895_read_register(BQ25895_REG_ADC_VBAT, &adc_vbat) ||
        !bq25895_read_register(BQ25895_REG_ADC_VBUS, &adc_vbus) ||
        !bq25895_read_register(BQ25895_REG_ADC_ICHG, &adc_ichg)) {
        return false;
    }
    
    // Parse status register (bits 4:3 = charging status)
    battery->charge_status = (status_reg >> 3) & 0x03;
    battery->is_charging = (battery->charge_status == 1 || battery->charge_status == 2);
    
    // Parse fault register
    battery->fault_status = fault_reg;
    battery->has_fault = (fault_reg != 0);
    
    // Convert ADC readings to real values
    battery->voltage_v = 2.304 + (adc_vbat & 0x7F) * 0.020;  // Battery voltage
    battery->bus_voltage_v = 2.600 + (adc_vbus & 0x7F) * 0.100;  // Bus voltage
    battery->charge_current_ma = (adc_ichg & 0x7F) * 50;  // Charge current
    
    battery->last_update = millis();
    battery->is_online = true;
    
    return true;
}

// ============================================================================
// CHARGING SYSTEM FUNCTIONS
// ============================================================================

void initialize_charging_system() {
    Serial.println("Initializing charging system...");
    
    // Initialize each charger
    system_status.active_chargers = 0;
    
    for (uint8_t i = 0; i < NUM_CHARGERS; i++) {
        uint8_t channel = charger_channels[i];
        
        Serial.printf("Initializing charger %d on channel %d...\n", i + 1, channel);
        
        if (tca9548_select_channel(channel)) {
            if (bq25895_initialize()) {
                batteries[i].channel = channel;
                batteries[i].is_online = true;
                system_status.active_chargers++;
                Serial.printf("Charger %d initialized successfully\n", i + 1);
            } else {
                batteries[i].is_online = false;
                Serial.printf("Charger %d initialization failed\n", i + 1);
            }
            tca9548_release_channel();
        } else {
            batteries[i].is_online = false;
            Serial.printf("Failed to select channel %d for charger %d\n", channel, i + 1);
        }
        
        delay(10);  // Brief pause between initializations
    }
    
    Serial.printf("Charging system initialized: %d/%d chargers active\n", 
                 system_status.active_chargers, NUM_CHARGERS);
}

void update_all_batteries() {
    static uint32_t last_update = 0;
    const uint32_t update_interval = 2000;  // 2 seconds
    
    if (millis() - last_update < update_interval) {
        return;
    }
    
    for (uint8_t i = 0; i < NUM_CHARGERS; i++) {
        if (!batteries[i].is_online) continue;
        
        uint8_t channel = charger_channels[i];
        
        if (tca9548_select_channel(channel)) {
            if (!bq25895_read_status(&batteries[i])) {
                batteries[i].is_online = false;
                Serial.printf("Lost communication with charger %d\n", i + 1);
            }
            tca9548_release_channel();
        }
        
        delay(1);  // Brief pause between readings
    }
    
    last_update = millis();
}

// ============================================================================
// SAFETY MONITORING
// ============================================================================

void check_safety() {
    bool system_fault = false;
    
    for (uint8_t i = 0; i < NUM_CHARGERS; i++) {
        if (!batteries[i].is_online) continue;
        
        BatteryData& battery = batteries[i];
        
        // Check critical safety limits
        if (battery.voltage_v > 4.25) {  // Overvoltage
            Serial.printf("CRITICAL: Battery %d overvoltage: %.2fV\n", i + 1, battery.voltage_v);
            system_fault = true;
        }
        
        if (battery.voltage_v < 2.5) {  // Severely undervoltage
            Serial.printf("CRITICAL: Battery %d undervoltage: %.2fV\n", i + 1, battery.voltage_v);
            system_fault = true;
        }
        
        if (battery.charge_current_ma > 1000) {  // Overcurrent
            Serial.printf("CRITICAL: Battery %d overcurrent: %.0fmA\n", i + 1, battery.charge_current_ma);
            system_fault = true;
        }
        
        if (battery.has_fault) {
            Serial.printf("WARNING: Battery %d fault register: 0x%02X\n", i + 1, battery.fault_status);
        }
    }
    
    system_status.system_healthy = !system_fault;
    
    if (system_fault) {
        // Could implement emergency shutdown here
        Serial.println("SYSTEM FAULT DETECTED - Monitor closely");
    }
}

// ============================================================================
// WEB SERVER FUNCTIONS
// ============================================================================

String get_charge_status_string(uint8_t status) {
    switch (status) {
        case 0: return "Not Charging";
        case 1: return "Pre-charge";
        case 2: return "Fast Charge";
        case 3: return "Charge Done";
        default: return "Unknown";
    }
}

float calculate_battery_percentage(float voltage) {
    // LiPo voltage to percentage approximation
    if (voltage >= 4.2) return 100.0;
    if (voltage <= 3.0) return 0.0;
    return ((voltage - 3.0) / 1.2) * 100.0;
}

void setup_web_server() {
    // Initialize SPIFFS for web files storage
    if (!SPIFFS.begin(true)) {
        Serial.println("SPIFFS Mount Failed");
    }
    
    // Serve main dashboard page
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "text/html", R"html(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Battery Charging Monitor</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body { 
            font-family: Arial, sans-serif; 
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            min-height: 100vh; padding: 20px; 
        }
        .header { text-align: center; color: white; margin-bottom: 30px; }
        .system-status { 
            background: rgba(255,255,255,0.95); border-radius: 10px; 
            padding: 15px; margin-bottom: 20px; 
            display: flex; justify-content: space-between; align-items: center; 
        }
        .battery-grid { 
            display: grid; grid-template-columns: repeat(auto-fit, minmax(300px, 1fr)); 
            gap: 20px; 
        }
        .battery-card { 
            background: rgba(255,255,255,0.95); border-radius: 15px; 
            padding: 25px; box-shadow: 0 8px 32px rgba(0,0,0,0.1); 
        }
        .battery-header { 
            display: flex; justify-content: space-between; 
            align-items: center; margin-bottom: 20px; 
        }
        .battery-visual { 
            width: 60px; height: 100px; border: 3px solid #333; 
            border-radius: 6px; position: relative; margin: 0 auto 20px; 
            background: #f5f5f5; 
        }
        .battery-level { 
            position: absolute; bottom: 0; width: 100%; 
            border-radius: 3px; transition: all 0.5s ease; 
        }
        .battery-terminal { 
            position: absolute; top: -6px; left: 50%; transform: translateX(-50%); 
            width: 20px; height: 6px; background: #333; border-radius: 2px; 
        }
        .level-high { background: linear-gradient(to top, #4CAF50, #8BC34A); }
        .level-medium { background: linear-gradient(to top, #FF9800, #FFC107); }
        .level-low { background: linear-gradient(to top, #F44336, #FF5722); }
        .level-charging { 
            background: linear-gradient(to top, #2196F3, #03A9F4); 
            animation: pulse 2s infinite; 
        }
        @keyframes pulse { 0%, 100% { opacity: 1; } 50% { opacity: 0.7; } }
        .battery-stats { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; }
        .stat-item { 
            text-align: center; padding: 10px; 
            background: #f8f9fa; border-radius: 8px; 
        }
        .stat-label { font-size: 0.8em; color: #666; margin-bottom: 5px; }
        .stat-value { font-size: 1.1em; font-weight: bold; }
        .status-normal { color: #4CAF50; }
        .status-warning { color: #FF9800; }
        .status-critical { color: #F44336; }
        .offline { opacity: 0.5; filter: grayscale(50%); }
    </style>
</head>
<body>
    <div class="header">
        <h1>ESP32-C6 Battery Charging Monitor</h1>
        <p>Real-time monitoring of 3 LiPo batteries @ 0.5A each</p>
    </div>
    
    <div class="system-status" id="systemStatus">
        <div>System: <span id="systemHealth">--</span></div>
        <div>Uptime: <span id="uptime">--</span></div>
        <div>Active: <span id="activeChargers">--</span>/3</div>
        <div>WiFi: <span id="wifiStrength">--</span> dBm</div>
    </div>
    
    <div class="battery-grid" id="batteryGrid">
        <!-- Battery cards populated by JavaScript -->
    </div>

    <script>
        let updateInterval;
        
        async function updateDashboard() {
            try {
                const [batteryResponse, systemResponse] = await Promise.all([
                    fetch('/api/batteries'),
                    fetch('/api/system')
                ]);
                
                if (batteryResponse.ok && systemResponse.ok) {
                    const batteryData = await batteryResponse.json();
                    const systemData = await systemResponse.json();
                    
                    updateSystemStatus(systemData);
                    updateBatteryGrid(batteryData.batteries);
                }
            } catch (error) {
                console.error('Dashboard update failed:', error);
            }
        }
        
        function updateSystemStatus(data) {
            document.getElementById('systemHealth').textContent = data.healthy ? 'Healthy' : 'Fault';
            document.getElementById('systemHealth').className = data.healthy ? 'status-normal' : 'status-critical';
            document.getElementById('uptime').textContent = formatUptime(data.uptime);
            document.getElementById('activeChargers').textContent = data.activeChargers;
            document.getElementById('wifiStrength').textContent = data.wifiRSSI;
        }
        
        function updateBatteryGrid(batteries) {
            const grid = document.getElementById('batteryGrid');
            grid.innerHTML = '';
            
            batteries.forEach((battery, index) => {
                const card = createBatteryCard(battery, index + 1);
                grid.appendChild(card);
            });
        }
        
        function createBatteryCard(battery, number) {
            const card = document.createElement('div');
            card.className = 'battery-card' + (battery.online ? '' : ' offline');
            
            const percentage = calculatePercentage(battery.voltage);
            const levelClass = getLevelClass(percentage, battery.charging);
            const statusClass = battery.fault ? 'status-critical' : (battery.charging ? 'status-normal' : 'status-warning');
            
            card.innerHTML = `
                <div class="battery-header">
                    <h3>Battery ${number}</h3>
                    <span class="${statusClass}">${battery.status}</span>
                </div>
                
                <div class="battery-visual">
                    <div class="battery-terminal"></div>
                    <div class="battery-level ${levelClass}" style="height: ${Math.max(5, percentage)}%"></div>
                </div>
                
                <div class="battery-stats">
                    <div class="stat-item">
                        <div class="stat-label">Voltage</div>
                        <div class="stat-value">${battery.voltage.toFixed(2)}V</div>
                    </div>
                    <div class="stat-item">
                        <div class="stat-label">Current</div>
                        <div class="stat-value">${battery.current.toFixed(0)}mA</div>
                    </div>
                    <div class="stat-item">
                        <div class="stat-label">Charge Level</div>
                        <div class="stat-value">${percentage.toFixed(0)}%</div>
                    </div>
                    <div class="stat-item">
                        <div class="stat-label">Input</div>
                        <div class="stat-value">${battery.busVoltage.toFixed(1)}V</div>
                    </div>
                </div>
            `;
            
            return card;
        }
        
        function calculatePercentage(voltage) {
            if (voltage >= 4.2) return 100;
            if (voltage <= 3.0) return 0;
            return ((voltage - 3.0) / 1.2) * 100;
        }
        
        function getLevelClass(percentage, charging) {
            if (charging) return 'level-charging';
            if (percentage > 60) return 'level-high';
            if (percentage > 20) return 'level-medium';
            return 'level-low';
        }
        
        function formatUptime(ms) {
            const seconds = Math.floor(ms / 1000);
            const hours = Math.floor(seconds / 3600);
            const minutes = Math.floor((seconds % 3600) / 60);
            const secs = seconds % 60;
            return `${hours.toString().padStart(2, '0')}:${minutes.toString().padStart(2, '0')}:${secs.toString().padStart(2, '0')}`;
        }
        
        // Start updating dashboard
        updateDashboard();
        updateInterval = setInterval(updateDashboard, 2000);
    </script>
</body>
</html>
        )html");
    });
    
    // API endpoint for battery data
    server.on("/api/batteries", HTTP_GET, [](AsyncWebServerRequest *request) {
        DynamicJsonDocument doc(2048);
        JsonArray battery_array = doc.createNestedArray("batteries");
        
        for (uint8_t i = 0; i < NUM_CHARGERS; i++) {
            JsonObject battery = battery_array.createNestedObject();
            
            battery["id"] = i;
            battery["channel"] = batteries[i].channel;
            battery["voltage"] = batteries[i].voltage_v;
            battery["current"] = batteries[i].charge_current_ma;
            battery["busVoltage"] = batteries[i].bus_voltage_v;
            battery["charging"] = batteries[i].is_charging;
            battery["fault"] = batteries[i].has_fault;
            battery["faultCode"] = batteries[i].fault_status;
            battery["status"] = get_charge_status_string(batteries[i].charge_status);
            battery["online"] = batteries[i].is_online;
            battery["lastUpdate"] = batteries[i].last_update;
        }
        
        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
    });
    
    // API endpoint for system status
    server.on("/api/system", HTTP_GET, [](AsyncWebServerRequest *request) {
        DynamicJsonDocument doc(512);
        
        doc["uptime"] = millis();
        doc["activeChargers"] = system_status.active_chargers;
        doc["wifiRSSI"] = WiFi.RSSI();
        doc["freeHeap"] = ESP.getFreeHeap();
        doc["healthy"] = system_status.system_healthy;
        
        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
    });
    
    server.begin();
    Serial.println("Web server started on http://" + WiFi.localIP().toString());
}

// ============================================================================
// MAIN SETUP AND LOOP
// ============================================================================

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("\n=== ESP32-C6 Triple Battery Charging System ===");
    Serial.println("Hardware: ESP32-C6 + TCA9548APWR + 3x BQ25895RTW");
    Serial.println("Target: 3x 1300mAh LiPo @ 0.5A each");
    
    // Initialize I2C
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    Wire.setClock(100000);  // 100kHz for reliability
    Serial.printf("I2C initialized: SDA=%d, SCL=%d, Clock=100kHz\n", I2C_SDA_PIN, I2C_SCL_PIN);
    
    // Create I2C mutex for thread safety
    i2c_mutex = xSemaphoreCreateMutex();
    if (i2c_mutex == NULL) {
        Serial.println("Failed to create I2C mutex!");
        while (1) delay(1000);
    }
    
    // Scan I2C channels to verify hardware
    tca9548_scan_channels();
    
    // Initialize charging system
    initialize_charging_system();
    
    // Connect to WiFi
    Serial.printf("Connecting to WiFi: %s", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    
    uint8_t wifi_attempts = 0;
    while (WiFi.status() != WL_CONNECTED && wifi_attempts < 30) {
        delay(500);
        Serial.print(".");
        wifi_attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\nWiFi connected: %s\n", WiFi.localIP().toString().c_str());
        Serial.printf("RSSI: %d dBm\n", WiFi.RSSI());
    } else {
        Serial.println("\nWiFi connection failed!");
    }
    
    // Initialize web server
    setup_web_server();
    
    // Initialize system status
    system_status.uptime_ms = millis();
    system_status.system_healthy = true;
    
    Serial.println("\n=== System Ready ===");
    Serial.println("Web dashboard: http://" + WiFi.localIP().toString());
    Serial.println("API endpoints: /api/batteries, /api/system");
    Serial.println("========================\n");
}

void loop() {
    static uint32_t last_status_print = 0;
    static uint32_t last_safety_check = 0;
    
    // Update battery readings
    update_all_batteries();
    
    // Safety monitoring every 5 seconds
    if (millis() - last_safety_check > 5000) {
        check_safety();
        last_safety_check = millis();
    }
    
    // Print status summary every 10 seconds
    if (millis() - last_status_print > 10000) {
        Serial.println("\n--- Battery Status Summary ---");
        for (uint8_t i = 0; i < NUM_CHARGERS; i++) {
            if (batteries[i].is_online) {
                Serial.printf("Battery %d: %.2fV, %.0fmA, %s, Ch%d\n", 
                             i + 1, 
                             batteries[i].voltage_v,
                             batteries[i].charge_current_ma,
                             get_charge_status_string(batteries[i].charge_status).c_str(),
                             batteries[i].channel);
            } else {
                Serial.printf("Battery %d: OFFLINE\n", i + 1);
            }
        }
        Serial.printf("System: %s, WiFi: %d dBm, Heap: %d bytes\n", 
                     system_status.system_healthy ? "Healthy" : "Fault",
                     WiFi.RSSI(), 
                     ESP.getFreeHeap());
        Serial.println("-----------------------------\n");
        
        last_status_print = millis();
    }
    
    // Main loop delay
    delay(100);
}