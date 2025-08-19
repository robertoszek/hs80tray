#include <KStatusNotifierItem>
#include <QApplication>
#include <QThread>
#include <QPainter>
#include <QPainterPath>
#include <QLoggingCategory>
#include <QMenu>
#include <QIcon>
#include <hidapi/hidapi.h>
#include <iostream>
#include <sstream>

#define VENDOR_ID  0x1B1C
#define PRODUCT_ID 0x0A73
#define BATTERY_LEVEL_EVENT 0x0F
#define CHARGING_EVENT 0x10
#define POLL_INTERVAL_MS 200
#define MAX_STR 255

static bool g_verbose = false;
static bool g_switch_sink = false;
static int last_charging = false;
static double last_percentage = -1;
static const int PASSIVE_TIMEOUT_MS = 30 * 60 * 1000;
static QString sink;

struct Sink {
    std::string id;
    std::string name;
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
    void startPolling() {
        hid_init();
        bool connected = false;
        handle = nullptr;
        while (!handle && !QThread::currentThread()->isInterruptionRequested()) {
            handle = hid_open(VENDOR_ID, PRODUCT_ID, nullptr);
            if (!handle) {
                std::wcout << L"Unable to open HID device, retrying in 30 seconds..." << std::endl;
                std::wcout << L"Is the Wireless Receiver connected?" << std::endl;
                QThread::sleep(30);
            }
        }

        // timer to keep track of how long ago we got the last msg
        QElapsedTimer lastMessageTimer;
        lastMessageTimer.start();
        unsigned char buf[65] = {0};
        if(handle){
            wchar_t wstr[MAX_STR];
            hid_get_manufacturer_string(handle, wstr, MAX_STR);
            std::wcout << L"Manufacturer: " << wstr << std::endl;
            hid_get_product_string(handle, wstr, MAX_STR);
            std::wcout << L"Product: " << wstr << std::endl;
            hid_get_serial_number_string(handle, wstr, MAX_STR);
            std::wcout << L"Serial Number: " << wstr << std::endl;

            uint8_t req[65] = {0};
            req[0] = 0x02;   // Report ID
            req[1] = 0x08;   // Command group?
            req[2] = 0x09;   // Subcommand: "get status" (inferred)
            hid_write(handle, req, sizeof(req));  // or 64 on some backends
        }

        while (true) {
            if(QThread::currentThread()->isInterruptionRequested()){
                return;
            }
            int res = hid_read_timeout(handle, buf, sizeof(buf), POLL_INTERVAL_MS);
            if (res > 0) {
                lastMessageTimer.restart();

                QByteArray data(reinterpret_cast<const char*>(buf), res);
                if(g_verbose) qDebug() << "Report ID" << QString::number(buf[0], 16)
                         << "response:" << data.toHex(' ');
                double percentage;
                bool charging;
                switch (buf[3]) {
                    case BATTERY_LEVEL_EVENT:
                    {
                        if (!connected){
                            connected = true;
                            if(g_switch_sink) switch_pipewire_sink(sink);
                        }
                        // only show tray icon if we have battery info
                        emit trayActive();
                        percentage = (buf[5] | (buf[6] << 8)) / 10;
                        last_percentage = percentage;
                        if(g_verbose) qDebug() << "Battery:" << percentage << "%";
                    } break;
                    case CHARGING_EVENT:
                    {
                        if (!connected){
                            connected = true;
                            if(g_switch_sink) switch_pipewire_sink(sink);
                        }
                        charging = (buf[5] == 1);
                        last_charging = charging;
                        if(g_verbose) qDebug() << "Charging: " << (charging ? "true" : "false");
                    } break;
                    default:
                    {} break;
                }
                emit batteryUpdated(last_percentage, last_charging);
            }
            else
            {
                // No message received after timeout, assuming headset disconnected
                if (lastMessageTimer.hasExpired(PASSIVE_TIMEOUT_MS)) {
                    emit trayPassive();
                    connected = false;
                }
            }
        }
    }

private:
    hid_device* handle = nullptr;
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

static std::string device;
int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
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

    QMenu *menu = new QMenu();
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

    // Worker thread for HID polling
    QThread *workerThread = new QThread();
    HIDWorker* worker = new HIDWorker;
    worker->moveToThread(workerThread);

    QObject::connect(workerThread, &QThread::started, worker, &HIDWorker::startPolling);
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

