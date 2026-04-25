#include <KStatusNotifierItem>
#include <QApplication>
#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFormLayout>
#include <QIcon>
#include <QLoggingCategory>
#include <QMenu>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSettings>
#include <QThread>
#include <QVBoxLayout>
#include <hidapi/hidapi.h>
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <iostream>
#include <sstream>
#include <vector>

#define VENDOR_ID  0x1B1C
#define PRODUCT_ID 0x0A6B
#define BATTERY_LEVEL_EVENT 0x0F
#define CHARGING_EVENT 0x10
#define MIC_STATUS_EVENT 0xA6
#define POLL_INTERVAL_MS 200
#define MAX_STR 255
#define REPORT_SIZE 65
#define HEADSET_MODE_WIRELESS 0x09

static bool g_verbose = false;
static bool g_switch_sink = false;
static int last_charging = false;
static double last_percentage = -1;
static const int PASSIVE_TIMEOUT_MS = 30 * 60 * 1000;
static const int INITIAL_BATTERY_REQUEST_DELAY_MS = 1000;
static const int UNKNOWN_BATTERY_REQUEST_INTERVAL_MS = 2000;
static const int BATTERY_REQUEST_INTERVAL_MS = 60 * 1000;
static const int MIC_REQUEST_INTERVAL_MS = 1000;
static const int STATUS_REQUEST_INTERVAL_MS = 10 * 1000;
static const int LIGHTING_HANDSHAKE_INTERVAL_MS = 10 * 1000;
static const int LED_KEEPALIVE_INTERVAL_MS = 2 * 1000;
static const int LED_MIN_UPDATE_INTERVAL_MS = 250;
static const int HEADSET_RESPONSE_TIMEOUT_MS = 3000;
static QString sink;

struct Sink {
    std::string id;
    std::string name;
};

struct LedConfig {
    QColor logoColor;
    QColor powerColor;
    QColor micMutedColor;
    QColor micUnmutedColor;
};

Q_DECLARE_METATYPE(LedConfig)

static LedConfig defaultLedConfig() {
    return {
        QColor(0, 0, 0),
        QColor(0, 255, 0),
        QColor(255, 0, 0),
        QColor(0, 0, 0),
    };
}

static QColor readSettingColor(QSettings& settings, const char* key, const QColor& fallback) {
    if (!settings.contains(key)) return fallback;

    const QVariant value = settings.value(key);
    QColor color = value.value<QColor>();
    if (!color.isValid()) color = QColor(value.toString());
    return color.isValid() ? color : fallback;
}

static QColor normalizeMicColor(const QColor& color, const QColor& fallback) {
    const QRgb rgb = color.rgb();
    if (rgb == QColor(0, 0, 0).rgb()
        || rgb == QColor(255, 0, 0).rgb()
        || rgb == QColor(255, 255, 255).rgb()) {
        return color;
    }
    return fallback;
}

static LedConfig loadLedConfig() {
    QSettings settings;
    const LedConfig defaults = defaultLedConfig();
    return {
        readSettingColor(settings, "logoColor", defaults.logoColor),
        readSettingColor(settings, "powerColor", defaults.powerColor),
        normalizeMicColor(readSettingColor(settings, "micMutedColor", defaults.micMutedColor), defaults.micMutedColor),
        normalizeMicColor(readSettingColor(settings, "micUnmutedColor", defaults.micUnmutedColor), defaults.micUnmutedColor),
    };
}

static void saveLedConfig(const LedConfig& config) {
    QSettings settings;
    settings.setValue("logoColor", config.logoColor);
    settings.setValue("powerColor", config.powerColor);
    settings.setValue("micMutedColor", config.micMutedColor);
    settings.setValue("micUnmutedColor", config.micUnmutedColor);
}

class LedSettingsWindow : public QWidget {
    Q_OBJECT
public:
    explicit LedSettingsWindow(const LedConfig& initialConfig, QWidget* parent = nullptr)
        : QWidget(parent), config(initialConfig) {
        setWindowTitle("HS80 LED");
        setWindowIcon(QIcon::fromTheme("preferences-desktop-display"));

        auto* layout = new QVBoxLayout(this);
        auto* form = new QFormLayout();
        layout->addLayout(form);

        powerButton = createColorButton(config.powerColor);
        logoButton = createColorButton(config.logoColor);
        form->addRow("Power LED", powerButton);
        form->addRow("Logo LED", logoButton);

        micMutedCombo = createMicCombo(config.micMutedColor, QColor(255, 0, 0));
        micUnmutedCombo = createMicCombo(config.micUnmutedColor, QColor(0, 0, 0));
        form->addRow("Mic muted", micMutedCombo);
        form->addRow("Mic unmuted", micUnmutedCombo);

        connect(powerButton, &QPushButton::clicked, this, [this]() {
            chooseColor("Power LED", config.powerColor, [this](const QColor& color) {
                config.powerColor = color;
                updateColorButton(powerButton, color);
            });
        });

        connect(logoButton, &QPushButton::clicked, this, [this]() {
            chooseColor("Logo LED", config.logoColor, [this](const QColor& color) {
                config.logoColor = color;
                updateColorButton(logoButton, color);
            });
        });

        connect(micMutedCombo, &QComboBox::currentIndexChanged, this, [this](int) {
            config.micMutedColor = micColorFromCombo(micMutedCombo);
            persistAndNotify();
        });

        connect(micUnmutedCombo, &QComboBox::currentIndexChanged, this, [this](int) {
            config.micUnmutedColor = micColorFromCombo(micUnmutedCombo);
            persistAndNotify();
        });

        setMinimumWidth(320);
    }

signals:
    void ledConfigChanged(const LedConfig& config);

protected:
    void closeEvent(QCloseEvent* event) override {
        hide();
        event->ignore();
    }

private:
    LedConfig config;
    QPushButton* powerButton = nullptr;
    QPushButton* logoButton = nullptr;
    QComboBox* micMutedCombo = nullptr;
    QComboBox* micUnmutedCombo = nullptr;

    QPushButton* createColorButton(const QColor& color) {
        auto* button = new QPushButton(this);
        button->setMinimumWidth(120);
        updateColorButton(button, color);
        return button;
    }

    void updateColorButton(QPushButton* button, const QColor& color) {
        button->setText(color.name(QColor::HexRgb).toUpper());
        button->setStyleSheet(QString(
            "QPushButton { background-color: %1; color: %2; border: 1px solid palette(mid); padding: 6px 10px; }")
                                  .arg(color.name(QColor::HexRgb),
                                       color.lightness() < 128 ? "#ffffff" : "#000000"));
    }

    void chooseColor(const QString& title, const QColor& currentColor, const std::function<void(const QColor&)>& applyColor) {
        const QColor color = QColorDialog::getColor(currentColor, this, title);
        if (!color.isValid()) return;

        applyColor(color);
        persistAndNotify();
    }

    QComboBox* createMicCombo(const QColor& color, const QColor& fallback) {
        auto* combo = new QComboBox(this);
        combo->addItem("Spento", QColor(0, 0, 0));
        combo->addItem("Rosso", QColor(255, 0, 0));
        combo->addItem("Bianco", QColor(255, 255, 255));

        const int index = comboIndexForColor(combo, color);
        combo->setCurrentIndex(index >= 0 ? index : comboIndexForColor(combo, fallback));
        return combo;
    }

    int comboIndexForColor(const QComboBox* combo, const QColor& color) const {
        for (int i = 0; i < combo->count(); ++i) {
            if (combo->itemData(i).value<QColor>().rgb() == color.rgb()) return i;
        }
        return -1;
    }

    QColor micColorFromCombo(const QComboBox* combo) const {
        return combo->currentData().value<QColor>();
    }

    void persistAndNotify() {
        saveLedConfig(config);
        emit ledConfigChanged(config);
    }
};

std::vector<Sink> get_sinks() {
    //TODO: use pipewire API instead of running cmds
    std::vector<Sink> sinks;
    std::array<char, 256> buffer;
    FILE* pipe = popen("pactl list short sinks", "r");
    if (!pipe) return sinks;

    while (fgets(buffer.data(), buffer.size(), pipe)) {
        std::string line(buffer.data());
        std::istringstream iss(line);
        Sink s;
        iss >> s.id >> s.name; // first two columns: ID and NAME
        sinks.push_back(s);
    }
    pclose(pipe);
    return sinks;
}

std::string find_sink_id(const std::string &target_name) {
    auto sinks = get_sinks();
    for (const auto &s : sinks) {
        if (s.name == target_name) {
            return s.id;
        }
    }
    return "";
}

int switch_pipewire_sink(const QString sink_name){
    std::string sink_str = sink_name.toStdString();
    std::string sink_id = find_sink_id(sink_str);

    if (sink_id.empty()) {
        std::cerr << "Sink not found: " << sink_str << "\n";
        return 1;
    }
    //TODO: use pipewire API instead of running cmds
    std::string cmd = "pactl set-default-sink " + sink_id;
    int ret = std::system(cmd.c_str());
    if (ret == 0) {
        std::cout << "Switched default sink to: " << sink_str
                  << " (ID=" << sink_id << ")\n";
    } else {
        std::cerr << "Failed to switch sink\n";
    }

    return ret;
}

// --- Worker to run in its own thread so blocking operations do not affect the main UI thread ---
class HIDWorker : public QObject {
    Q_OBJECT
public:
    HIDWorker(QObject* parent = nullptr) : QObject(parent) {}
    ~HIDWorker() {
        if (handle) {
            hid_close(handle);
            hid_exit();
        }
    }

signals:
    void batteryUpdated(double percentage, bool charging);
    void trayPassive();
    void trayActive();

public slots:
    void updateLedConfig(const LedConfig& config) {
        QMutexLocker locker(&ledConfigMutex);
        ledConfig = config;
        ledDirty = true;
    }

    void startPolling() {
        hid_init();
        bool connected = false;
        bool headsetResponding = true;
        bool micMuted = false;
        bool lightingDirty = true;
        handle = nullptr;
        while (!handle && !QThread::currentThread()->isInterruptionRequested()) {
            handle = openHs80Interface();
            if (!handle) {
                std::wcout << L"Unable to open HID device, retrying in 30 seconds..." << std::endl;
                std::wcout << L"Is the Wireless Receiver connected?" << std::endl;
                QThread::sleep(30);
            }
        }

        // timer to keep track of how long ago we got the last msg
        QElapsedTimer lastMessageTimer;
        lastMessageTimer.start();
        QElapsedTimer lastBatteryRequestTimer;
        lastBatteryRequestTimer.start();
        QElapsedTimer lastMicRequestTimer;
        lastMicRequestTimer.start();
        QElapsedTimer lastStatusRequestTimer;
        lastStatusRequestTimer.start();
        QElapsedTimer lastLightingTimer;
        lastLightingTimer.start();
        QElapsedTimer lastLedTimer;
        lastLedTimer.start();
        
        unsigned char buf[REPORT_SIZE] = {0};
        if(handle){
            wchar_t wstr[MAX_STR];
            hid_get_manufacturer_string(handle, wstr, MAX_STR);
            std::wcout << L"Manufacturer: " << wstr << std::endl;
            hid_get_product_string(handle, wstr, MAX_STR);
            std::wcout << L"Product: " << wstr << std::endl;
            hid_get_serial_number_string(handle, wstr, MAX_STR);
            std::wcout << L"Serial Number: " << wstr << std::endl;
        }

        QThread::msleep(INITIAL_BATTERY_REQUEST_DELAY_MS);
        configureLighting();
        clearLedDirty();
        sendLedState(micMuted);
        lightingDirty = false;
        lastLightingTimer.restart();
        lastLedTimer.restart();
        requestBattery();
        requestMicStatus();

        while (true) {
            if(QThread::currentThread()->isInterruptionRequested()){
                return;
            }

            if (lastLightingTimer.elapsed() > LIGHTING_HANDSHAKE_INTERVAL_MS) {
                lightingDirty = true;
            }

            if (lastMicRequestTimer.elapsed() > MIC_REQUEST_INTERVAL_MS) {
                requestMicStatus();
                lastMicRequestTimer.restart();
            }

            if (lastStatusRequestTimer.elapsed() > STATUS_REQUEST_INTERVAL_MS) {
                requestSleepStatus();
                lastStatusRequestTimer.restart();
            }

            int batteryRequestInterval = last_percentage > 0
                                             ? BATTERY_REQUEST_INTERVAL_MS
                                             : UNKNOWN_BATTERY_REQUEST_INTERVAL_MS;
            if (lastBatteryRequestTimer.elapsed() > batteryRequestInterval) {
                requestBattery();
                lastBatteryRequestTimer.restart();
            }

            bool ledKeepaliveDue = lastLedTimer.elapsed() > LED_KEEPALIVE_INTERVAL_MS;
            bool ledChangeDue = (isLedDirty() || lightingDirty) && lastLedTimer.elapsed() > LED_MIN_UPDATE_INTERVAL_MS;
            if (ledKeepaliveDue || ledChangeDue) {
                if (lightingDirty) {
                    configureLighting();
                    lightingDirty = false;
                    lastLightingTimer.restart();
                }
                clearLedDirty();
                sendLedState(micMuted);
                lastLedTimer.restart();
            }

            int res = hid_read_timeout(handle, buf, sizeof(buf), POLL_INTERVAL_MS);
            if (res > 0) {
                bool refreshAfterResponse = false;
                if (!headsetResponding) {
                    lightingDirty = true;
                    markLedDirty();
                    refreshAfterResponse = true;
                    if(g_verbose) qDebug() << "Headset response restored, lighting resync queued";
                }
                headsetResponding = true;
                lastMessageTimer.restart();

                QByteArray data(reinterpret_cast<const char*>(buf), res);
                if(g_verbose) qDebug() << "Report ID" << QString::number(buf[0], 16)
                         << "response:" << data.toHex(' ');
                
                double percentage = last_percentage;
                bool charging = last_charging;
                
                uint8_t eventType = buf[3];
                
                if (eventType == BATTERY_LEVEL_EVENT) {
                    double new_p = readBatteryPercentage(buf, res);
                    if (new_p > 0 && new_p <= 100) {
                        percentage = new_p;
                        if(g_verbose) qDebug() << "Battery Event - Percentage:" << percentage << "%";
                    }
                } else if (eventType == CHARGING_EVENT) {
                    charging = parseChargingState(buf, res);
                    if(g_verbose) qDebug() << "Charging Event - State:" << (charging ? "Charging" : "Battery/Full");
                } else if (eventType == MIC_STATUS_EVENT) {
                    bool newMicMuted = parseMicMuted(buf, res);
                    if (newMicMuted != micMuted) {
                        micMuted = newMicMuted;
                        markLedDirty();
                    }
                    if(g_verbose) qDebug() << "Mic Event - State:" << (micMuted ? "Muted" : "Active");
                } else if (isQueryResponse(buf, res)) {
                    double responsePercentage = readQueryResponseBatteryPercentage(buf, res);
                    if (responsePercentage > 0) {
                        discardPendingQuery(QueryKind::BatteryLevel);
                        percentage = responsePercentage;
                        if(g_verbose) qDebug() << "Battery Response - Percentage:" << percentage << "%";
                    } else if (isQueryResponseMicState(buf, res) && peekPendingQuery() == QueryKind::MicStatus) {
                        takePendingQuery();
                        bool newMicMuted = parseMicMuted(buf, res);
                        if (newMicMuted != micMuted) {
                            micMuted = newMicMuted;
                            markLedDirty();
                        }
                        if(g_verbose) qDebug() << "Mic Response - State:" << (micMuted ? "Muted" : "Active");
                    } else if (isQueryResponseChargingState(buf, res)) {
                        discardPendingQuery(QueryKind::BatteryStatus);
                        discardPendingQuery(QueryKind::SleepStatus);
                        charging = parseChargingState(buf, res);
                        if(g_verbose) qDebug() << "Charging Response - State:" << (charging ? "Charging" : "Battery/Full");
                    }
                }

                if (percentage != last_percentage || charging != last_charging || !connected) {
                    if (!connected && (percentage > 0)) {
                        connected = true;
                        if(g_switch_sink) switch_pipewire_sink(sink);
                        emit trayActive();
                    }
                    if (percentage > 0) last_percentage = percentage;
                    last_charging = charging;
                    emit batteryUpdated(last_percentage, last_charging);
                }

                if (refreshAfterResponse) {
                    requestBattery();
                    requestMicStatus();
                }
            }
            else
            {
                if (lastMessageTimer.hasExpired(HEADSET_RESPONSE_TIMEOUT_MS)) {
                    headsetResponding = false;
                }

                // No message received after timeout, assuming headset disconnected
                if (lastMessageTimer.hasExpired(PASSIVE_TIMEOUT_MS)) {
                    if (connected) {
                        emit trayPassive();
                        connected = false;
                        last_percentage = -1;
                    }
                }
            }
        }
    }

private:
    enum class QueryKind {
        None,
        BatteryLevel,
        BatteryStatus,
        SleepStatus,
        MicStatus,
    };

    hid_device* handle = nullptr;
    std::deque<QueryKind> pendingQueries;
    QMutex ledConfigMutex;
    LedConfig ledConfig = defaultLedConfig();
    bool ledDirty = true;

    bool isLedDirty() {
        QMutexLocker locker(&ledConfigMutex);
        return ledDirty;
    }

    void markLedDirty() {
        QMutexLocker locker(&ledConfigMutex);
        ledDirty = true;
    }

    void clearLedDirty() {
        QMutexLocker locker(&ledConfigMutex);
        ledDirty = false;
    }

    LedConfig currentLedConfig() {
        QMutexLocker locker(&ledConfigMutex);
        return ledConfig;
    }

    hid_device* openHs80Interface() {
        hid_device_info* devs = hid_enumerate(VENDOR_ID, PRODUCT_ID);
        hid_device_info* curDev = devs;
        hid_device* opened = nullptr;

        while (curDev) {
            if (curDev->interface_number == 3) {
                opened = hid_open_path(curDev->path);
                if (opened) break;
            }
            curDev = curDev->next;
        }

        hid_free_enumeration(devs);
        return opened;
    }

    void writePacket(const std::vector<uint8_t>& packet) {
        if (!handle) return;

        uint8_t buf[REPORT_SIZE] = {0};
        for (size_t i = 0; i < packet.size() && i < REPORT_SIZE; ++i) {
            buf[i] = packet[i];
        }

        int res = hid_write(handle, buf, sizeof(buf));
        if(g_verbose && res < 0) qDebug() << "hid_write failed";
    }

    void drainReadBuffer() {
        if (!handle) return;

        uint8_t buf[REPORT_SIZE] = {0};
        while (hid_read_timeout(handle, buf, sizeof(buf), 0) > 0) {}
        pendingQueries.clear();
    }

    void writeQuery(const std::vector<uint8_t>& packet, QueryKind kind) {
        writePacket(packet);
        pendingQueries.push_back(kind);
    }

    QueryKind takePendingQuery() {
        if (pendingQueries.empty()) return QueryKind::None;

        QueryKind kind = pendingQueries.front();
        pendingQueries.pop_front();
        return kind;
    }

    QueryKind peekPendingQuery() const {
        if (pendingQueries.empty()) return QueryKind::None;

        return pendingQueries.front();
    }

    void discardPendingQuery(QueryKind kind) {
        for (auto it = pendingQueries.begin(); it != pendingQueries.end(); ++it) {
            if (*it == kind) {
                pendingQueries.erase(it);
                return;
            }
        }
    }

    void configureLighting() {
        writePacket({0x02, HEADSET_MODE_WIRELESS, 0x01, 0x03, 0x00, 0x02});
        writePacket({0x02, HEADSET_MODE_WIRELESS, 0x0D, 0x00, 0x01});
        if(g_verbose) qDebug() << "Lighting endpoint configured";
    }

    void requestBattery() {
        drainReadBuffer();
        writeQuery({0x02, HEADSET_MODE_WIRELESS, 0x02, BATTERY_LEVEL_EVENT, 0x00}, QueryKind::BatteryLevel);
        QThread::msleep(50);
        writeQuery({0x02, HEADSET_MODE_WIRELESS, 0x02, CHARGING_EVENT, 0x00}, QueryKind::BatteryStatus);
        if(g_verbose) qDebug() << "Battery request sent";
    }

    void requestMicStatus() {
        writeQuery({0x02, HEADSET_MODE_WIRELESS, 0x02, MIC_STATUS_EVENT, 0x00}, QueryKind::MicStatus);
        if(g_verbose) qDebug() << "Mic status request sent";
    }

    void requestSleepStatus() {
        writeQuery({0x02, HEADSET_MODE_WIRELESS, 0x02, CHARGING_EVENT, 0x00}, QueryKind::SleepStatus);
    }

    void sendLedState(bool micMuted) {
        const LedConfig config = currentLedConfig();
        const QColor micColor = micMuted ? config.micMutedColor : config.micUnmutedColor;

        uint8_t buf[REPORT_SIZE] = {0};
        buf[0] = 0x02;
        buf[1] = HEADSET_MODE_WIRELESS;
        buf[2] = 0x06;
        buf[3] = 0x00;
        buf[4] = 0x09;
        buf[8] = static_cast<uint8_t>(config.logoColor.red());
        buf[9] = static_cast<uint8_t>(config.powerColor.red());
        buf[10] = static_cast<uint8_t>(micColor.red());
        buf[11] = static_cast<uint8_t>(config.logoColor.green());
        buf[12] = static_cast<uint8_t>(config.powerColor.green());
        buf[13] = static_cast<uint8_t>(micColor.green());
        buf[14] = static_cast<uint8_t>(config.logoColor.blue());
        buf[15] = static_cast<uint8_t>(config.powerColor.blue());
        buf[16] = static_cast<uint8_t>(micColor.blue());

        if (handle) hid_write(handle, buf, sizeof(buf));
        if(g_verbose) qDebug() << "LED state sent:" << (micMuted ? "muted" : "active");
    }

    double readBatteryPercentage(const uint8_t* buf, int res) {
        if (res > 6) {
            double eventValue = (buf[5] | (buf[6] << 8)) / 10.0;
            if (eventValue > 0 && eventValue <= 100) return eventValue;
        }

        if (res > 5) {
            double responseValue = (buf[4] | (buf[5] << 8)) / 10.0;
            if (responseValue > 0 && responseValue <= 100) return responseValue;
        }

        return -1;
    }

    bool isQueryResponse(const uint8_t* buf, int res) {
        return res > 5 && buf[0] == 0x01 && buf[1] == 0x01 && buf[2] == 0x02 && buf[3] == 0x00;
    }

    double readQueryResponseBatteryPercentage(const uint8_t* buf, int res) {
        if (!isQueryResponse(buf, res)) return -1;

        int rawValue = buf[4] | (buf[5] << 8);
        if (rawValue > 100 && rawValue <= 1000) {
            return rawValue / 10.0;
        }

        return -1;
    }

    bool isQueryResponseChargingState(const uint8_t* buf, int res) {
        return isQueryResponse(buf, res) && buf[4] >= 1 && buf[4] <= 3 && buf[5] == 0;
    }

    bool isQueryResponseMicState(const uint8_t* buf, int res) {
        return isQueryResponse(buf, res) && (buf[4] == 0 || buf[4] == 1) && buf[5] == 0;
    }

    bool parseChargingState(const uint8_t* buf, int res) {
        uint8_t state = 0;
        if (res > 5 && buf[5] >= 1 && buf[5] <= 3) {
            state = buf[5];
        } else if (res > 4) {
            state = buf[4];
        }

        return state == 1;
    }

    bool parseMicMuted(const uint8_t* buf, int res) {
        if (isQueryResponse(buf, res) && res > 4) {
            return buf[4] == 1;
        }

        if (res > 5 && (buf[5] == 0 || buf[5] == 1)) {
            return buf[5] == 1;
        }

        if (res > 4) {
            return buf[4] == 1;
        }

        return false;
    }

};

QIcon getBatteryIcon(double percentage, bool charging) {
    if (percentage < 0 || percentage > 100) {
        if(g_verbose) qDebug() << "Percentage out of bounds: " << percentage;
        QIcon charging_unknown_percentage_icon = QIcon::fromTheme("battery-full-charging-symbolic");
        QIcon unknown_percentage_icon = QIcon::fromTheme("battery-missing-symbolic");
        return charging ? charging_unknown_percentage_icon : unknown_percentage_icon;
    }
    int level = ((int)percentage / 10) * 10; // round down to nearest 10

    QString iconName = QString("battery-%1%2")
                           .arg(level, 3, 10, QChar('0'))
                           .arg(charging ? "-charging" : "");
    if(g_verbose) qDebug() << "Icon: " << iconName.toStdString().c_str() << " Percentage: " << level;
    QIcon baseIcon = QIcon::fromTheme(iconName);
    QString percentText = QString("%1%").arg(static_cast<int>(percentage));

    int iconSize = 128;
    QPixmap pixmap = baseIcon.pixmap(iconSize, iconSize);

    // Paint percentage text on top
    QPainter painter(&pixmap);
    QFont font("Arial", iconSize / 3, QFont::Bold); // scale font relative to icon
    painter.setFont(font);

    // Outline
    QPainterPath path;
    path.addText(pixmap.width() - painter.fontMetrics().horizontalAdvance(percentText) - 5,
                 pixmap.height() - 5,
                 font,
                 percentText);

    // Black outline
    QPen outlinePen(Qt::black, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.setPen(outlinePen);
    painter.drawPath(path);

    painter.fillPath(path, Qt::white);

    return QIcon(pixmap);
}

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setOrganizationName("hs80tray");
    QApplication::setApplicationName("hs80tray");
    app.setQuitOnLastWindowClosed(false);
    qRegisterMetaType<LedConfig>("LedConfig");

    QStringList args = app.arguments();
    for (int i = 1; i < args.size(); ++i) {
        if (args[i] == "-h" || args[i] == "--help") {
            std::cout << "Usage: " << args[0].toStdString() << " [options]\n";
            std::cout << "Show a battery indicator on the system tray for Corsair HS80 headset\n";
            std::cout << "Options:\n";
            std::cout << "  -h, --help      Show this help message\n";
            std::cout << "  -d, --device    Switch to sink name when connecting headset\n";
            std::cout << "  -v, --verbose   Enable verbose debug output\n";
            return 0;
        }
        if (args[i] == "-v" || args[i] == "--verbose") {
            g_verbose = true;
        }
        if (args[i] == "-d" || args[i] == "--device") {
            sink = args[i+1];
            g_switch_sink = true;
        }
    }
    QLoggingCategory::defaultCategory()->setEnabled(QtDebugMsg, true);

    KStatusNotifierItem tray;
    tray.setCategory(KStatusNotifierItem::Hardware);
    tray.setStatus(KStatusNotifierItem::Passive);
    QIcon initialIcon = QIcon::fromTheme("battery-missing-symbolic");
    tray.setIconByPixmap(initialIcon.pixmap(64, 64));
    tray.setToolTipTitle("HS80 Battery");
    tray.setToolTipSubTitle("Initializing...");
    QWidget qwidget = QWidget();
    QMenu *menu = new QMenu(&qwidget);
    QAction* header = menu->addAction("Corsair HS80");
    header->setIcon(QIcon::fromTheme("audio-headset"));
    header->setDisabled(true); // prevent clicking
    QFont headerFont = header->font();
    header->setFont(headerFont);
    menu->addSeparator();

    QAction* batteryAction = menu->addAction("Battery: --%");
    batteryAction->setIcon(QIcon::fromTheme("battery-symbolic"));
    QAction* chargingAction = menu->addAction("Status: Unknown");
    chargingAction->setIcon(QIcon::fromTheme("ac-adapter"));
    menu->addSeparator();
    tray.setContextMenu(menu);

    const LedConfig initialLedConfig = loadLedConfig();
    LedSettingsWindow ledSettingsWindow(initialLedConfig);
    QObject::connect(&tray, &KStatusNotifierItem::activateRequested,
                     &app,
                     [&ledSettingsWindow](bool, const QPoint&) {
        ledSettingsWindow.show();
        ledSettingsWindow.raise();
        ledSettingsWindow.activateWindow();
    });

    // Worker thread for HID polling
    QThread *workerThread = new QThread();
    HIDWorker* worker = new HIDWorker;
    worker->updateLedConfig(initialLedConfig);
    worker->moveToThread(workerThread);

    QObject::connect(workerThread, &QThread::started, worker, &HIDWorker::startPolling);
    QObject::connect(&ledSettingsWindow, &LedSettingsWindow::ledConfigChanged,
                     worker, &HIDWorker::updateLedConfig,
                     Qt::DirectConnection);
    QObject::connect(worker, &HIDWorker::batteryUpdated,
                     &app,
                     [&tray, batteryAction, chargingAction](double percentage, bool charging){
        QIcon batteryIcon = getBatteryIcon(percentage, charging);
        tray.setIconByPixmap(batteryIcon.pixmap(64, 64));
        // Update menu actions
        if(percentage > 0) batteryAction->setText(QString("Battery: %1%").arg(int(percentage)));
        chargingAction->setText(QString("Status: %1").arg(charging ? "Charging" : "Discharging"));

        tray.setToolTipTitle("HS80 Battery");
        tray.setToolTipSubTitle(QString("%1% %2")
                                    .arg(percentage, 0, 'f', 0)
                                    .arg(charging ? "Charging" : "Discharging"));
    });

    QObject::connect(worker, &HIDWorker::trayPassive,
                     &app,
                     [trayPtr = &tray](){
        trayPtr->setStatus(KStatusNotifierItem::Passive);
    });

    QObject::connect(worker, &HIDWorker::trayActive,
                     &app,
                     [trayPtr = &tray](){
        trayPtr->setStatus(KStatusNotifierItem::Active);
    });

    QObject::connect(&app, &QApplication::aboutToQuit, &app, [&](){
        if(g_verbose) qDebug() << "change da world my final message. Goodb ye";
        workerThread->quit();
        workerThread->requestInterruption();
        workerThread->wait();
        worker->deleteLater();
        workerThread->deleteLater();
    });

    workerThread->start();
    return app.exec();
}

#include "main.moc"
