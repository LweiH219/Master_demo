#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QCheckBox>
#include <QDebug>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLibraryInfo>
#include <QMainWindow>
#include <QMessageBox>
#include <QNetworkInterface>
#include <QPlainTextEdit>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSet>
#include <QHash>
#include <QSpinBox>
#include <QStyle>
#include <QTabWidget>
#include <QTextCursor>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <algorithm>
#include <cmath>

namespace {

class MotorPlot final : public QWidget {
public:
    explicit MotorPlot(QWidget *parent = nullptr) : QWidget(parent) {
        setMinimumHeight(230);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }
    void setMode(int mode) { mode_ = mode; update(); }
    void addSample(double position, double target, double rpm, double rpmTarget,
                   double current, double output) {
        samples_.append({position, target, rpm, rpmTarget, current, output});
        while (samples_.size() > 600) samples_.removeFirst();
        update();
    }
    void clear() { samples_.clear(); update(); }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(), Qt::white);
        const QRectF area = QRectF(rect()).adjusted(58, 20, -18, -30);
        p.setPen(QPen(QColor("#e4e7ec"), 1));
        for (int i = 0; i <= 4; ++i) {
            const qreal y = area.top() + area.height() * i / 4.0;
            p.drawLine(QPointF(area.left(), y), QPointF(area.right(), y));
        }
        if (samples_.isEmpty()) {
            p.setPen(QColor("#667085"));
            p.drawText(area, Qt::AlignCenter, QStringLiteral("启动 DJI 调试后，实时波形将在此显示"));
            return;
        }
        QVector<double> a, b;
        QString unit;
        if (mode_ == 0) {
            for (const auto &s : samples_) { a << s.position; b << s.target; }
            unit = QStringLiteral("位置 (°)");
        } else if (mode_ == 1) {
            for (const auto &s : samples_) { a << s.rpm; b << s.rpmTarget; }
            unit = QStringLiteral("转速 (rpm)");
        } else {
            for (const auto &s : samples_) { a << s.current; b << s.output; }
            unit = QStringLiteral("反馈 / 输出 (原始计数)");
        }
        double lo = std::min(*std::min_element(a.cbegin(), a.cend()), *std::min_element(b.cbegin(), b.cend()));
        double hi = std::max(*std::max_element(a.cbegin(), a.cend()), *std::max_element(b.cbegin(), b.cend()));
        if (hi - lo < 1e-6) { lo -= 1.0; hi += 1.0; }
        const double pad = (hi - lo) * 0.1;
        lo -= pad; hi += pad;
        p.setPen(QColor("#667085"));
        p.drawText(QRectF(4, area.top(), 50, 20), Qt::AlignLeft, unit);
        p.drawText(QRectF(4, area.top() - 3, 50, 18), Qt::AlignRight, QString::number(hi, 'g', 4));
        p.drawText(QRectF(4, area.bottom() - 15, 50, 18), Qt::AlignRight, QString::number(lo, 'g', 4));
        const auto drawSeries = [&](const QVector<double> &values, const QColor &color) {
            QPainterPath path;
            for (int i = 0; i < values.size(); ++i) {
                const qreal x = area.left() + area.width() * i / std::max(1, values.size() - 1);
                const qreal y = area.bottom() - (values[i] - lo) / (hi - lo) * area.height();
                if (i == 0) path.moveTo(x, y); else path.lineTo(x, y);
            }
            p.setPen(QPen(color, 2)); p.drawPath(path);
        };
        drawSeries(a, QColor("#175cd3"));
        drawSeries(b, QColor("#f79009"));
        p.setPen(QColor("#344054"));
        const QString first = mode_ == 2 ? QStringLiteral("反馈") : QStringLiteral("实测");
        const QString second = mode_ == 2 ? QStringLiteral("输出") : QStringLiteral("目标");
        p.setBrush(QColor("#175cd3")); p.drawEllipse(QPointF(area.left() + 6, height() - 13), 4, 4);
        p.drawText(QRectF(area.left() + 14, height() - 22, 65, 18), Qt::AlignLeft, first);
        p.setBrush(QColor("#f79009")); p.drawEllipse(QPointF(area.left() + 88, height() - 13), 4, 4);
        p.drawText(QRectF(area.left() + 96, height() - 22, 65, 18), Qt::AlignLeft, second);
    }
private:
    struct Sample { double position, target, rpm, rpmTarget, current, output; };
    QVector<Sample> samples_;
    int mode_ = 0;
};

QPushButton *makeButton(const QString &text, QWidget *parent,
                        const QString &kind = QString())
{
    auto *button = new QPushButton(text, parent);
    if (!kind.isEmpty()) button->setProperty("kind", kind);
    button->setMinimumHeight(38);
    return button;
}

QFrame *makeCard(const QString &title, const QString &description,
                 QVBoxLayout *parentLayout, QWidget *parent)
{
    auto *card = new QFrame(parent);
    card->setObjectName("card");
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(20, 18, 20, 20);
    layout->setSpacing(12);

    auto *heading = new QLabel(title, card);
    heading->setObjectName("cardTitle");
    auto *subtitle = new QLabel(description, card);
    subtitle->setObjectName("muted");
    subtitle->setWordWrap(true);
    layout->addWidget(heading);
    layout->addWidget(subtitle);
    if (parentLayout) parentLayout->addWidget(card);
    return card;
}

QSpinBox *makeSpin(QWidget *parent, int min, int max, int value,
                   const QString &suffix = QString())
{
    auto *spin = new QSpinBox(parent);
    spin->setRange(min, max);
    spin->setValue(value);
    spin->setSuffix(suffix);
    return spin;
}

class MasterWindow final : public QMainWindow {
public:
    MasterWindow()
        : process_(new QProcess(this)), interfaceBox_(new QComboBox(this)),
          status_(new QLabel(QStringLiteral("就绪"), this)),
          output_(new QPlainTextEdit(this)), stopButton_(nullptr),
          interfaceHint_(nullptr), killTimer_(new QTimer(this))
    {
        killTimer_->setSingleShot(true);
        process_->setProcessChannelMode(QProcess::MergedChannels);
        setWindowTitle(QStringLiteral("ECAT2CAN · 测试工作台"));
        setMinimumSize(900, 620);
        resize(1220, 850);
        setStyleSheet(QStringLiteral(R"(
            QWidget { font-family: "Noto Sans CJK SC", "Microsoft YaHei", sans-serif; color: #182230; }
            QMainWindow { background: #f3f6fb; }
            #topBar { background: white; border-bottom: 1px solid #e4e7ec; }
            #brand { font-size: 22px; font-weight: 700; color: #101828; }
            #muted { color: #667085; font-size: 12px; }
            #sectionTitle { font-size: 15px; font-weight: 650; }
            #card { background: white; border: 1px solid #e4e7ec; border-radius: 11px; }
            #cardTitle { font-size: 15px; font-weight: 650; }
            #status { border-radius: 12px; padding: 7px 12px; background: #ecfdf3; color: #027a48; font-weight: 600; }
            #status[busy="true"] { background: #fffaeb; color: #b54708; }
            #status[error="true"] { background: #fef3f2; color: #b42318; }
            QTabWidget::pane { border: 0; background: transparent; }
            QTabBar::tab { background: transparent; color: #667085; padding: 12px 18px; margin-right: 4px; }
            QTabBar::tab:selected { color: #175cd3; border-bottom: 2px solid #175cd3; font-weight: 650; }
            QPushButton { background: white; border: 1px solid #d0d5dd; border-radius: 7px; padding: 7px 13px; }
            QPushButton:hover { background: #f2f4f7; }
            QPushButton:disabled { color: #98a2b3; background: #f2f4f7; }
            QPushButton[kind="primary"] { color: white; background: #175cd3; border-color: #175cd3; font-weight: 600; }
            QPushButton[kind="primary"]:hover { background: #1849a9; }
            QPushButton[kind="danger"] { color: #b42318; border-color: #fecdca; }
            QComboBox, QSpinBox, QLineEdit { min-height: 34px; background: white; border: 1px solid #d0d5dd; border-radius: 6px; padding: 2px 8px; }
            QPlainTextEdit { background: #101828; color: #d0d5dd; border: 0; border-radius: 8px; padding: 10px; font-family: "Noto Sans Mono", monospace; font-size: 12px; }
            QDockWidget::title { background: #f3f6fb; padding: 8px 12px; font-weight: 650; }
        )"));

        buildUi();
        refreshInterfaces();
        connectProcess();
    }

private:
    QProcess *process_;
    QComboBox *interfaceBox_;
    QLabel *status_;
    QPlainTextEdit *output_;
    QPushButton *stopButton_;
    QLabel *interfaceHint_;
    QTimer *killTimer_;

    QSpinBox *loopCycles_ = nullptr;
    QSpinBox *scanCycles_ = nullptr;
    QSpinBox *feedbackCycles_ = nullptr;
    QComboBox *canChannel_ = nullptr;
    QSpinBox *txFrequency_ = nullptr;
    QSpinBox *txFrameCount_ = nullptr;
    QSpinBox *hostSeconds_ = nullptr;
    QSpinBox *rtSeconds_ = nullptr;
    QComboBox *rtChannel_ = nullptr;
    QSpinBox *benchCycles_ = nullptr;
    QComboBox *benchChannel_ = nullptr;
    QComboBox *latTxChannel_ = nullptr;
    QComboBox *latRxChannel_ = nullptr;
    QSpinBox *latSeconds_ = nullptr;
    QSpinBox *latFrequency_ = nullptr;
    QSpinBox *latLoad_ = nullptr;
    QComboBox *advancedCommand_ = nullptr;
    QLineEdit *advancedArgs_ = nullptr;
    QLabel *advancedHelp_ = nullptr;
    QComboBox *motorKind_ = nullptr;
    QComboBox *motorMode_ = nullptr;
    QComboBox *motorChannel_ = nullptr;
    QSpinBox *motorId_ = nullptr;
    QDoubleSpinBox *motorRatio_ = nullptr;
    QDoubleSpinBox *motorLimit_ = nullptr;
    QCheckBox *gmVoltage_ = nullptr;
    QLineEdit *motorCommand_ = nullptr;
    MotorPlot *motorPlot_ = nullptr;
    QComboBox *plotMode_ = nullptr;
    QString processBuffer_;
    bool motorProcess_ = false;

    bool isRunning() const { return process_->state() != QProcess::NotRunning; }

    QString selectedInterface() const { return interfaceBox_->currentData().toString(); }

    QComboBox *makeChannelBox(QWidget *parent, int initial = 3)
    {
        auto *box = new QComboBox(parent);
        for (int channel = 0; channel <= 3; ++channel)
            box->addItem(QStringLiteral("MCAN%1 · 通道 %1").arg(channel), channel);
        box->setCurrentIndex(initial);
        return box;
    }

    void buildUi()
    {
        auto *root = new QWidget(this);
        auto *rootLayout = new QVBoxLayout(root);
        rootLayout->setContentsMargins(0, 0, 0, 0);
        rootLayout->setSpacing(0);

        auto *top = new QWidget(root);
        top->setObjectName("topBar");
        auto *topLayout = new QHBoxLayout(top);
        topLayout->setContentsMargins(24, 16, 24, 16);
        auto *brandBox = new QVBoxLayout();
        auto *brand = new QLabel(QStringLiteral("ECAT2CAN"), top);
        brand->setObjectName("brand");
        auto *subtitle = new QLabel(QStringLiteral("从站测试 · 诊断 · 性能分析"), top);
        subtitle->setObjectName("muted");
        brandBox->addWidget(brand);
        brandBox->addWidget(subtitle);
        topLayout->addLayout(brandBox);
        topLayout->addStretch();
        topLayout->addWidget(new QLabel(QStringLiteral("EtherCAT 网卡"), top));
        interfaceBox_->setMinimumWidth(220);
        topLayout->addWidget(interfaceBox_);
        auto *refresh = makeButton(QStringLiteral("刷新"), top);
        topLayout->addWidget(refresh);
        interfaceHint_ = new QLabel(top);
        interfaceHint_->setObjectName("muted");
        interfaceHint_->setMinimumWidth(175);
        topLayout->addWidget(interfaceHint_);
        status_->setObjectName("status");
        topLayout->addWidget(status_);
        rootLayout->addWidget(top);

        auto *tabs = new QTabWidget(root);
        tabs->setDocumentMode(true);
        tabs->addTab(makeDiagnosticsPage(tabs), QStringLiteral("从站诊断"));
        tabs->addTab(makeCanPage(tabs), QStringLiteral("CAN 测试"));
        tabs->addTab(makePerformancePage(tabs), QStringLiteral("性能测试"));
        tabs->addTab(makeMotorPage(tabs), QStringLiteral("DJI 电机"));
        tabs->addTab(makeAdvancedPage(tabs), QStringLiteral("高级命令"));
        rootLayout->addWidget(tabs, 1);
        setCentralWidget(root);

        auto *dock = new QDockWidget(QStringLiteral("执行输出"), this);
        dock->setObjectName(QStringLiteral("outputDock"));
        dock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
        auto *outputPanel = new QWidget(dock);
        auto *outputLayout = new QVBoxLayout(outputPanel);
        outputLayout->setContentsMargins(12, 4, 12, 10);
        output_->setReadOnly(true);
        output_->setMaximumBlockCount(5000);
        output_->setMinimumHeight(105);
        output_->setPlaceholderText(QStringLiteral("运行结果、诊断信息和错误提示会显示在这里。"));
        auto *outputControls = new QHBoxLayout();
        auto *clear = makeButton(QStringLiteral("清空日志"), outputPanel);
        stopButton_ = makeButton(QStringLiteral("停止任务"), outputPanel, "danger");
        stopButton_->setEnabled(false);
        outputControls->addStretch();
        outputControls->addWidget(clear);
        outputControls->addWidget(stopButton_);
        outputLayout->addWidget(output_);
        outputLayout->addLayout(outputControls);
        dock->setWidget(outputPanel);
        addDockWidget(Qt::BottomDockWidgetArea, dock);
        resizeDocks({dock}, {235}, Qt::Vertical);
        connect(refresh, &QPushButton::clicked, this, [this] { refreshInterfaces(); });
        connect(interfaceBox_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this](int) { updateInterfaceHint(); });
        connect(clear, &QPushButton::clicked, output_, &QPlainTextEdit::clear);
        connect(stopButton_, &QPushButton::clicked, this, [this] { stopProcess(); });
    }

    QWidget *makeDiagnosticsPage(QWidget *parent)
    {
        auto *page = new QWidget(parent);
        auto *layout = new QVBoxLayout(page);
        layout->setContentsMargins(24, 20, 24, 24);
        layout->setSpacing(16);
        auto *intro = new QLabel(QStringLiteral("先确认从站在线，再执行过程数据和寄存器检查。"), page);
        intro->setObjectName("muted");
        layout->addWidget(intro);

        auto *grid = new QGridLayout();
        grid->setSpacing(14);
        auto *stateCard = new QFrame(page);
        stateCard->setObjectName("card");
        auto *stateLayout = new QVBoxLayout(stateCard);
        stateLayout->setContentsMargins(20, 18, 20, 20);
        stateLayout->setSpacing(12);
        auto *stateTitle = new QLabel(QStringLiteral("从站状态"), stateCard);
        stateTitle->setObjectName("cardTitle");
        auto *stateDesc = new QLabel(QStringLiteral("进入 OP 并持续显示状态、WKC 和往返信息。"), stateCard);
        stateDesc->setObjectName("muted");
        stateDesc->setWordWrap(true);
        auto *info = makeButton(QStringLiteral("启动状态监视"), stateCard, "primary");
        stateLayout->addWidget(stateTitle);
        stateLayout->addWidget(stateDesc);
        stateLayout->addStretch();
        stateLayout->addWidget(info);
        connect(info, &QPushButton::clicked, this, [this] { runMaster({"info"}); });
        grid->addWidget(stateCard, 0, 0);

        auto *pdoCard = new QFrame(page);
        pdoCard->setObjectName("card");
        auto *pdoLayout = new QVBoxLayout(pdoCard);
        pdoLayout->setContentsMargins(20, 18, 20, 20);
        pdoLayout->setSpacing(12);
        auto *pdoTitle = new QLabel(QStringLiteral("PDO 往返校验"), pdoCard);
        pdoTitle->setObjectName("cardTitle");
        auto *pdoDesc = new QLabel(QStringLiteral("验证从站 192 字节过程数据的往返一致性。"), pdoCard);
        pdoDesc->setObjectName("muted");
        auto *pdoForm = new QFormLayout();
        loopCycles_ = makeSpin(pdoCard, 1, 1000000, 1000, QStringLiteral(" 次"));
        pdoForm->addRow(QStringLiteral("往返次数"), loopCycles_);
        auto *loop = makeButton(QStringLiteral("运行校验"), pdoCard, "primary");
        pdoLayout->addWidget(pdoTitle);
        pdoLayout->addWidget(pdoDesc);
        pdoLayout->addLayout(pdoForm);
        pdoLayout->addWidget(loop);
        pdoLayout->addStretch();
        connect(loop, &QPushButton::clicked, this, [this] {
            runMaster({"loop", QString::number(loopCycles_->value())});
        });
        grid->addWidget(pdoCard, 0, 1);

        auto *regCard = makeCard(QStringLiteral("ESC 寄存器"),
                                 QStringLiteral("读取链路状态和站地址，用于快速排查 EtherCAT 物理链路。"),
                                 nullptr, page);
        auto *regRow = new QHBoxLayout();
        auto *regs = makeButton(QStringLiteral("读取寄存器"), regCard, "primary");
        auto *reset = makeButton(QStringLiteral("复位从站"), regCard, "danger");
        regRow->addWidget(regs);
        regRow->addWidget(reset);
        regRow->addStretch();
        regCard->layout()->addItem(regRow);
        connect(regs, &QPushButton::clicked, this, [this] { runMaster({"regs"}); });
        connect(reset, &QPushButton::clicked, this, [this] {
            const auto answer = QMessageBox::warning(
                this, QStringLiteral("确认复位从站"),
                QStringLiteral("复位会使从站暂时离开 OP 状态，确认继续？"),
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
            if (answer == QMessageBox::Yes) runMaster({"reset"});
        });
        grid->addWidget(regCard, 1, 0, 1, 2);
        layout->addLayout(grid);
        layout->addStretch();
        return scrollPage(page, parent);
    }

    QWidget *makeCanPage(QWidget *parent)
    {
        auto *page = new QWidget(parent);
        auto *layout = new QVBoxLayout(page);
        layout->setContentsMargins(24, 20, 24, 24);
        layout->setSpacing(16);
        auto *intro = new QLabel(QStringLiteral("扫描和反馈读取为只读操作；发送操作会在执行前二次确认。"), page);
        intro->setObjectName("muted");
        layout->addWidget(intro);

        auto *grid = new QGridLayout();
        grid->setSpacing(14);
        auto *scanCard = new QFrame(page);
        scanCard->setObjectName("card");
        auto *scanLayout = new QVBoxLayout(scanCard);
        scanLayout->setContentsMargins(20, 18, 20, 20);
        scanLayout->setSpacing(12);
        auto *scanTitle = new QLabel(QStringLiteral("通道扫描"), scanCard);
        scanTitle->setObjectName("cardTitle");
        auto *scanDesc = new QLabel(QStringLiteral("被动监听并统计各 CAN 通道收到的帧和 ID。"), scanCard);
        scanDesc->setObjectName("muted");
        auto *scanForm = new QFormLayout();
        scanCycles_ = makeSpin(scanCard, 1, 1000000, 3000, QStringLiteral(" 次"));
        scanForm->addRow(QStringLiteral("采样次数"), scanCycles_);
        auto *scan = makeButton(QStringLiteral("开始扫描"), scanCard, "primary");
        scanLayout->addWidget(scanTitle);
        scanLayout->addWidget(scanDesc);
        scanLayout->addLayout(scanForm);
        scanLayout->addWidget(scan);
        scanLayout->addStretch();
        connect(scan, &QPushButton::clicked, this, [this] {
            runMaster({"scan", QString::number(scanCycles_->value())});
        });
        grid->addWidget(scanCard, 0, 0);

        auto *fdbCard = new QFrame(page);
        fdbCard->setObjectName("card");
        auto *fdbLayout = new QVBoxLayout(fdbCard);
        fdbLayout->setContentsMargins(20, 18, 20, 20);
        fdbLayout->setSpacing(12);
        auto *fdbTitle = new QLabel(QStringLiteral("CAN 反馈"), fdbCard);
        fdbTitle->setObjectName("cardTitle");
        auto *fdbDesc = new QLabel(QStringLiteral("只读指定轮数，查看从站收到的反馈帧。"), fdbCard);
        fdbDesc->setObjectName("muted");
        auto *fdbForm = new QFormLayout();
        feedbackCycles_ = makeSpin(fdbCard, 1, 1000000, 20, QStringLiteral(" 次"));
        fdbForm->addRow(QStringLiteral("采样次数"), feedbackCycles_);
        auto *fdb = makeButton(QStringLiteral("读取反馈"), fdbCard);
        fdbLayout->addWidget(fdbTitle);
        fdbLayout->addWidget(fdbDesc);
        fdbLayout->addLayout(fdbForm);
        fdbLayout->addWidget(fdb);
        fdbLayout->addStretch();
        connect(fdb, &QPushButton::clicked, this, [this] {
            runMaster({"fdb", QString::number(feedbackCycles_->value())});
        });
        grid->addWidget(fdbCard, 0, 1);

        auto *txCard = new QFrame(page);
        txCard->setObjectName("card");
        auto *txLayout = new QVBoxLayout(txCard);
        txLayout->setContentsMargins(20, 18, 20, 20);
        txLayout->setSpacing(12);
        auto *txTitle = new QLabel(QStringLiteral("CAN 报文发送"), txCard);
        txTitle->setObjectName("cardTitle");
        auto *txDesc = new QLabel(QStringLiteral("设置发送频率和总帧数。支持 1–8 个十六进制数据字节。"), txCard);
        txDesc->setObjectName("muted");
        txDesc->setWordWrap(true);
        auto *txForm = new QFormLayout();
        canChannel_ = makeChannelBox(txCard);
        auto *id = new QLineEdit(QStringLiteral("0x123"), txCard);
        id->setPlaceholderText(QStringLiteral("0x000–0x7FF"));
        auto *data = new QLineEdit(QStringLiteral("11 22 33 44 55 66 77 88"), txCard);
        data->setPlaceholderText(QStringLiteral("例如：11 22 33"));
        txFrequency_ = makeSpin(txCard, 1, 1000, 1000, QStringLiteral(" Hz"));
        txFrameCount_ = makeSpin(txCard, 1, 1000000, 2000, QStringLiteral(" 帧"));
        txForm->addRow(QStringLiteral("发送通道"), canChannel_);
        txForm->addRow(QStringLiteral("标准帧 ID"), id);
        txForm->addRow(QStringLiteral("数据字节"), data);
        txForm->addRow(QStringLiteral("发送频率"), txFrequency_);
        txForm->addRow(QStringLiteral("发送帧数"), txFrameCount_);
        auto *buttons = new QHBoxLayout();
        auto *send = makeButton(QStringLiteral("确认并发送单通道"), txCard, "danger");
        auto *sendAll = makeButton(QStringLiteral("发送到全部 4 路"), txCard, "danger");
        buttons->addWidget(send);
        buttons->addWidget(sendAll);
        buttons->addStretch();
        txLayout->addWidget(txTitle);
        txLayout->addWidget(txDesc);
        txLayout->addLayout(txForm);
        txLayout->addLayout(buttons);
        connect(send, &QPushButton::clicked, this, [this, id, data] {
            QStringList args;
            if (!parseCanFrame(id, data, args)) return;
            args.prepend(QString::number(canChannel_->currentData().toInt()));
            args.prepend(QStringLiteral("work"));
            args << QStringLiteral("--hz=%1").arg(txFrequency_->value())
                 << QStringLiteral("--frames=%1").arg(txFrameCount_->value());
            const double duration = static_cast<double>(txFrameCount_->value()) / txFrequency_->value();
            const QString warning = QStringLiteral("即将从 MCAN%1 以 %2 Hz 发送 %3 帧（约 %4 秒），ID %5。确认总线与设备允许该报文。")
                                        .arg(canChannel_->currentData().toInt())
                                        .arg(txFrequency_->value())
                                        .arg(txFrameCount_->value())
                                        .arg(duration, 0, 'f', 1).arg(id->text());
            confirmAndRun(QStringLiteral("确认发送 CAN 报文"), warning, args);
        });
        connect(sendAll, &QPushButton::clicked, this, [this, id, data] {
            QStringList args;
            if (!parseCanFrame(id, data, args)) return;
            args.prepend(QStringLiteral("workall"));
            args << QStringLiteral("--hz=%1").arg(txFrequency_->value())
                 << QStringLiteral("--frames=%1").arg(txFrameCount_->value());
            const double duration = static_cast<double>(txFrameCount_->value()) / txFrequency_->value();
            confirmAndRun(QStringLiteral("确认向全部通道发送"),
                          QStringLiteral("此操作会以 %1 Hz 向 4 路 CAN 通道各发送 %2 帧（约 %3 秒）。")
                              .arg(txFrequency_->value()).arg(txFrameCount_->value())
                              .arg(duration, 0, 'f', 1), args);
        });
        grid->addWidget(txCard, 1, 0, 1, 2);
        layout->addLayout(grid);
        layout->addStretch();
        return scrollPage(page, parent);
    }

    QWidget *makePerformancePage(QWidget *parent)
    {
        auto *page = new QWidget(parent);
        auto *layout = new QVBoxLayout(page);
        layout->setContentsMargins(24, 20, 24, 24);
        layout->setSpacing(16);
        auto *intro = new QLabel(QStringLiteral("根据测试目的选择纯主机调度测试、EtherCAT 基准或双 CAN 回环延迟测试。"), page);
        intro->setObjectName("muted");
        layout->addWidget(intro);
        auto *grid = new QGridLayout();
        grid->setSpacing(14);

        auto *hostCard = new QFrame(page);
        hostCard->setObjectName("card");
        auto *hostLayout = new QVBoxLayout(hostCard);
        hostLayout->setContentsMargins(20, 18, 20, 20);
        hostLayout->setSpacing(12);
        auto *hostTitle = new QLabel(QStringLiteral("主机调度实时性"), hostCard);
        hostTitle->setObjectName("cardTitle");
        auto *hostDesc = new QLabel(QStringLiteral("不连接 EtherCAT 从站，统计 Linux 主机循环周期与抖动。"), hostCard);
        hostDesc->setObjectName("muted");
        hostSeconds_ = makeSpin(hostCard, 1, 3600, 10, QStringLiteral(" 秒"));
        auto *hostForm = new QFormLayout();
        hostForm->addRow(QStringLiteral("测试时长"), hostSeconds_);
        auto *host = makeButton(QStringLiteral("运行主机测试"), hostCard, "primary");
        hostLayout->addWidget(hostTitle);
        hostLayout->addWidget(hostDesc);
        hostLayout->addLayout(hostForm);
        hostLayout->addWidget(host);
        hostLayout->addStretch();
        connect(host, &QPushButton::clicked, this, [this] {
            runMaster({"rt", QString::number(hostSeconds_->value()), "host"}, false);
        });
        grid->addWidget(hostCard, 0, 0);

        auto *rtCard = new QFrame(page);
        rtCard->setObjectName("card");
        auto *rtLayout = new QVBoxLayout(rtCard);
        rtLayout->setContentsMargins(20, 18, 20, 20);
        rtLayout->setSpacing(12);
        auto *rtTitle = new QLabel(QStringLiteral("EtherCAT 实时性"), rtCard);
        rtTitle->setObjectName("cardTitle");
        auto *rtDesc = new QLabel(QStringLiteral("连接从站后运行，生成周期直方图和截止期统计。"), rtCard);
        rtDesc->setObjectName("muted");
        rtSeconds_ = makeSpin(rtCard, 1, 3600, 10, QStringLiteral(" 秒"));
        rtChannel_ = makeChannelBox(rtCard);
        auto *rtForm = new QFormLayout();
        rtForm->addRow(QStringLiteral("测试时长"), rtSeconds_);
        rtForm->addRow(QStringLiteral("CAN 通道"), rtChannel_);
        auto *rt = makeButton(QStringLiteral("运行链路实时性测试"), rtCard);
        rtLayout->addWidget(rtTitle);
        rtLayout->addWidget(rtDesc);
        rtLayout->addLayout(rtForm);
        rtLayout->addWidget(rt);
        rtLayout->addStretch();
        connect(rt, &QPushButton::clicked, this, [this] {
            runMaster({"rt", QString::number(rtSeconds_->value()),
                       QString::number(rtChannel_->currentData().toInt())});
        });
        grid->addWidget(rtCard, 0, 1);

        auto *benchCard = new QFrame(page);
        benchCard->setObjectName("card");
        auto *benchLayout = new QVBoxLayout(benchCard);
        benchLayout->setContentsMargins(20, 18, 20, 20);
        benchLayout->setSpacing(12);
        auto *benchTitle = new QLabel(QStringLiteral("主站链路基准"), benchCard);
        benchTitle->setObjectName("cardTitle");
        auto *benchDesc = new QLabel(QStringLiteral("测量 EtherCAT 往返、循环抖动和 CAN 吞吐；测试会产生总线流量。"), benchCard);
        benchDesc->setObjectName("muted");
        benchDesc->setWordWrap(true);
        benchCycles_ = makeSpin(benchCard, 1, 1000000, 5000, QStringLiteral(" 次"));
        benchChannel_ = makeChannelBox(benchCard);
        auto *benchForm = new QFormLayout();
        benchForm->addRow(QStringLiteral("测试次数"), benchCycles_);
        benchForm->addRow(QStringLiteral("CAN 通道"), benchChannel_);
        auto *bench = makeButton(QStringLiteral("确认并运行基准"), benchCard, "danger");
        benchLayout->addWidget(benchTitle);
        benchLayout->addWidget(benchDesc);
        benchLayout->addLayout(benchForm);
        benchLayout->addWidget(bench);
        benchLayout->addStretch();
        connect(bench, &QPushButton::clicked, this, [this] {
            confirmAndRun(QStringLiteral("链路性能测试"),
                          QStringLiteral("此测试会在 MCAN%1 上产生测试流量。请确认通道允许测试帧。")
                              .arg(benchChannel_->currentData().toInt()),
                          {"bench", QString::number(benchCycles_->value()),
                           QString::number(benchChannel_->currentData().toInt())});
        });
        grid->addWidget(benchCard, 1, 0);

        auto *latCard = new QFrame(page);
        latCard->setObjectName("card");
        auto *latLayout = new QVBoxLayout(latCard);
        latLayout->setContentsMargins(20, 18, 20, 20);
        latLayout->setSpacing(12);
        auto *latTitle = new QLabel(QStringLiteral("双 CAN 回环延迟"), latCard);
        latTitle->setObjectName("cardTitle");
        auto *latDesc = new QLabel(QStringLiteral("需要把发送和接收通道接在同一条 CAN 总线上。"), latCard);
        latDesc->setObjectName("muted");
        latTxChannel_ = makeChannelBox(latCard, 2);
        latRxChannel_ = makeChannelBox(latCard, 3);
        latSeconds_ = makeSpin(latCard, 1, 600, 30, QStringLiteral(" 秒"));
        latFrequency_ = makeSpin(latCard, 100, 10000, 1000, QStringLiteral(" Hz"));
        latLoad_ = makeSpin(latCard, 0, 16, 0, QStringLiteral(" 帧/拍"));
        auto *latForm = new QFormLayout();
        latForm->addRow(QStringLiteral("发送通道"), latTxChannel_);
        latForm->addRow(QStringLiteral("接收通道"), latRxChannel_);
        latForm->addRow(QStringLiteral("测试时长"), latSeconds_);
        latForm->addRow(QStringLiteral("发送频率"), latFrequency_);
        latForm->addRow(QStringLiteral("总线负载"), latLoad_);
        auto *lat = makeButton(QStringLiteral("确认并运行回环"), latCard, "danger");
        latLayout->addWidget(latTitle);
        latLayout->addWidget(latDesc);
        latLayout->addLayout(latForm);
        latLayout->addWidget(lat);
        connect(lat, &QPushButton::clicked, this, [this] {
            confirmAndRun(QStringLiteral("CAN 回环测试"),
                          QStringLiteral("该测试会持续发送 CAN 测试帧。确认通道接线正确且允许测试流量。"),
                          {"looplat", QString::number(latTxChannel_->currentData().toInt()),
                           QString::number(latRxChannel_->currentData().toInt()),
                           QString::number(latSeconds_->value()),
                           QString::number(latFrequency_->value()),
                           QString::number(latLoad_->value())});
        });
        grid->addWidget(latCard, 1, 1);
        layout->addLayout(grid);
        layout->addStretch();
        return scrollPage(page, parent);
    }

    QWidget *makeMotorPage(QWidget *parent)
    {
        auto *page = new QWidget(parent);
        auto *layout = new QVBoxLayout(page);
        layout->setContentsMargins(24, 20, 24, 24);
        layout->setSpacing(14);
        auto *intro = new QLabel(QStringLiteral("通过 motor_pos 调试 M3508/C620 与 GM6020。默认只读监视；可查看位置、转速和输出波形。"), page);
        intro->setObjectName("muted"); intro->setWordWrap(true);
        layout->addWidget(intro);

        auto *config = new QFrame(page); config->setObjectName("card");
        auto *configLayout = new QVBoxLayout(config);
        configLayout->setContentsMargins(20, 18, 20, 20); configLayout->setSpacing(12);
        auto *form = new QFormLayout();
        motorKind_ = new QComboBox(config);
        motorKind_->addItem(QStringLiteral("M3508 + C620"), QStringLiteral("m3508"));
        motorKind_->addItem(QStringLiteral("GM6020"), QStringLiteral("gm6020"));
        motorChannel_ = makeChannelBox(config, 3);
        motorId_ = makeSpin(config, 1, 8, 1);
        motorMode_ = new QComboBox(config);
        motorMode_->addItem(QStringLiteral("只读监视（禁止使能）"), true);
        motorMode_->addItem(QStringLiteral("控制模式（启动前确认）"), false);
        motorRatio_ = new QDoubleSpinBox(config); motorRatio_->setRange(0.01, 1000); motorRatio_->setDecimals(4); motorRatio_->setValue(19.2032);
        motorLimit_ = new QDoubleSpinBox(config); motorLimit_->setRange(0, 25000); motorLimit_->setDecimals(2); motorLimit_->setValue(3.0);
        gmVoltage_ = new QCheckBox(QStringLiteral("GM6020 使用电压模式（默认电流模式）"), config);
        gmVoltage_->setEnabled(false);
        form->addRow(QStringLiteral("电机类型"), motorKind_);
        form->addRow(QStringLiteral("CAN 通道"), motorChannel_);
        form->addRow(QStringLiteral("电机 / 驱动器 ID"), motorId_);
        form->addRow(QStringLiteral("运行方式"), motorMode_);
        form->addRow(QStringLiteral("减速比"), motorRatio_);
        form->addRow(QStringLiteral("输出限幅"), motorLimit_);
        configLayout->addLayout(form);
        configLayout->addWidget(gmVoltage_);
        auto *start = makeButton(QStringLiteral("启动 DJI 调试"), config, "primary");
        configLayout->addWidget(start, 0, Qt::AlignLeft);
        layout->addWidget(config);

        auto *plotCard = new QFrame(page); plotCard->setObjectName("card");
        auto *plotLayout = new QVBoxLayout(plotCard);
        plotLayout->setContentsMargins(18, 14, 18, 14); plotLayout->setSpacing(8);
        auto *plotHeader = new QHBoxLayout();
        auto *plotTitle = new QLabel(QStringLiteral("实时波形"), plotCard); plotTitle->setObjectName("cardTitle");
        plotMode_ = new QComboBox(plotCard);
        plotMode_->addItem(QStringLiteral("位置 / 目标"), 0);
        plotMode_->addItem(QStringLiteral("转速 / 目标转速"), 1);
        plotMode_->addItem(QStringLiteral("反馈 / 输出（原始计数）"), 2);
        plotHeader->addWidget(plotTitle); plotHeader->addStretch(); plotHeader->addWidget(plotMode_);
        motorPlot_ = new MotorPlot(plotCard);
        plotLayout->addLayout(plotHeader); plotLayout->addWidget(motorPlot_);
        layout->addWidget(plotCard, 1);

        auto *commands = new QFrame(page); commands->setObjectName("card");
        auto *commandLayout = new QVBoxLayout(commands);
        commandLayout->setContentsMargins(16, 12, 16, 12);
        auto *basicCommands = new QHBoxLayout();
        auto *advancedCommands = new QHBoxLayout();
        auto *enable = makeButton(QStringLiteral("使能 e"), commands, "danger");
        auto *disable = makeButton(QStringLiteral("失能 d"), commands);
        auto *softStop = makeButton(QStringLiteral("软停 s"), commands);
        auto *setZero = makeButton(QStringLiteral("设零 z"), commands);
        auto *target = new QDoubleSpinBox(commands); target->setRange(-3600, 3600); target->setDecimals(2); target->setSuffix(QStringLiteral(" °"));
        auto *setTarget = makeButton(QStringLiteral("设置目标"), commands);
        motorCommand_ = new QLineEdit(commands); motorCommand_->setPlaceholderText(QStringLiteral("高级命令：如 kp 8、c 1、g"));
        auto *send = makeButton(QStringLiteral("发送"), commands);
        basicCommands->addWidget(enable); basicCommands->addWidget(disable); basicCommands->addWidget(softStop); basicCommands->addWidget(setZero);
        basicCommands->addStretch(); basicCommands->addWidget(target); basicCommands->addWidget(setTarget);
        advancedCommands->addWidget(new QLabel(QStringLiteral("运行时命令"), commands));
        advancedCommands->addWidget(motorCommand_, 1); advancedCommands->addWidget(send);
        commandLayout->addLayout(basicCommands); commandLayout->addLayout(advancedCommands);
        layout->addWidget(commands);

        connect(motorKind_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
            const bool gm = motorKind_->currentData().toString() == QStringLiteral("gm6020");
            motorId_->setMaximum(gm ? 7 : 8);
            motorRatio_->setValue(gm ? 1.0 : 19.2032);
            motorLimit_->setMaximum(gm && gmVoltage_->isChecked() ? 25000 : (gm ? 3 : 20));
            motorLimit_->setValue(gm && gmVoltage_->isChecked() ? 3000 : (gm ? 1 : 3));
            gmVoltage_->setEnabled(gm);
        });
        connect(gmVoltage_, &QCheckBox::toggled, this, [this](bool voltage) {
            if (motorKind_->currentData().toString() == QStringLiteral("gm6020")) {
                motorLimit_->setMaximum(voltage ? 25000 : 3);
                motorLimit_->setValue(voltage ? 3000 : 1);
            }
        });
        connect(plotMode_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this](int) { motorPlot_->setMode(plotMode_->currentData().toInt()); });
        connect(start, &QPushButton::clicked, this, [this] { runMotor(); });
        connect(enable, &QPushButton::clicked, this, [this] {
            if (QMessageBox::warning(this, QStringLiteral("确认使能电机"),
                    QStringLiteral("电机即将进入闭环控制。请确认电机已固定、周围无人，且已先在只读模式核对反馈。"),
                    QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Yes)
                sendMotorCommand(QStringLiteral("e"));
        });
        connect(disable, &QPushButton::clicked, this, [this] { sendMotorCommand(QStringLiteral("d")); });
        connect(softStop, &QPushButton::clicked, this, [this] { sendMotorCommand(QStringLiteral("s")); });
        connect(setZero, &QPushButton::clicked, this, [this] { sendMotorCommand(QStringLiteral("z")); });
        connect(setTarget, &QPushButton::clicked, this, [this, target] { sendMotorCommand(QString::number(target->value(), 'f', 2)); });
        connect(send, &QPushButton::clicked, this, [this] { sendMotorCommand(motorCommand_->text().trimmed()); });
        connect(motorCommand_, &QLineEdit::returnPressed, send, &QPushButton::click);
        return scrollPage(page, parent);
    }

    void sendMotorCommand(const QString &command)
    {
        if (!motorProcess_ || !isRunning()) {
            QMessageBox::information(this, QStringLiteral("DJI 调试未运行"), QStringLiteral("请先启动 DJI 调试进程。"));
            return;
        }
        if (!command.isEmpty()) process_->write((command + QLatin1Char('\n')).toLocal8Bit());
    }

    void runMotor()
    {
        if (isRunning()) {
            QMessageBox::information(this, QStringLiteral("任务正在运行"), QStringLiteral("请先停止当前任务，再启动 DJI 调试。"));
            return;
        }
        const QString iface = selectedInterface();
        if (iface.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("缺少网卡"), QStringLiteral("请选择连接 ECAT2CAN 从站的网卡。"));
            return;
        }
        const QString executable = QDir(QApplication::applicationDirPath()).filePath(QStringLiteral("motor_pos"));
        if (!QFileInfo::exists(executable)) {
            QMessageBox::critical(this, QStringLiteral("找不到 DJI 程序"), QStringLiteral("未找到同目录下的 motor_pos：\n%1").arg(executable));
            return;
        }
        const QString kind = motorKind_->currentData().toString();
        const bool gm = kind == QStringLiteral("gm6020");
        QStringList args{iface, QStringLiteral("--motor"), kind, QStringLiteral("--ch"), QString::number(motorChannel_->currentData().toInt()),
                         QStringLiteral("--id"), QString::number(motorId_->value()), QStringLiteral("--ratio"), QString::number(motorRatio_->value(), 'f', 4),
                         QStringLiteral("--limit"), QString::number(motorLimit_->value(), 'f', 2),
                         QStringLiteral("--log-ms"), QStringLiteral("20"), QStringLiteral("--csv")};
        if (gm && gmVoltage_->isChecked()) args << QStringLiteral("--gm-voltage");
        const bool monitor = motorMode_->currentData().toBool();
        if (monitor) args << QStringLiteral("--monitor");
        else if (QMessageBox::warning(this, QStringLiteral("启动电机控制"),
                    QStringLiteral("这将启动可控电机进程，进程默认仍为失能；之后还需再次确认才能使能。\n请确认电机已固定并准备好急停。"),
                    QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;
        motorProcess_ = true; motorPlot_->clear();
        startProcess(executable, QStringList{iface} + args.mid(1), QStringLiteral("motor_pos"));
    }

    QWidget *makeAdvancedPage(QWidget *parent)
    {
        auto *page = new QWidget(parent);
        auto *layout = new QVBoxLayout(page);
        layout->setContentsMargins(24, 20, 24, 24);
        layout->setSpacing(16);
        auto *intro = new QLabel(QStringLiteral("直接调用主站支持的任意命令，便于使用暂未做成表单的诊断和测试功能。"), page);
        intro->setObjectName("muted");
        layout->addWidget(intro);

        auto *card = new QFrame(page);
        card->setObjectName("card");
        auto *cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(20, 18, 20, 20);
        cardLayout->setSpacing(14);
        auto *title = new QLabel(QStringLiteral("命令参数"), card);
        title->setObjectName("cardTitle");
        auto *form = new QFormLayout();
        advancedCommand_ = new QComboBox(card);
        const QList<QPair<QString, QString>> commands{
            {"regs · ESC 寄存器", "regs"}, {"info · 从站监视", "info"},
            {"loop · PDO 校验", "loop"}, {"scan · CAN 扫描", "scan"},
            {"work · 发送 CAN 帧", "work"}, {"workall · 全通道发送", "workall"},
            {"fdb · CAN 反馈", "fdb"}, {"bench · 链路基准", "bench"},
            {"rt · 实时性测试", "rt"}, {"bench_lat · 端到端延迟", "bench_lat"},
            {"looplat · CAN 回环延迟", "looplat"}, {"looplat2 · 多从站回环", "looplat2"},
            {"canloop · 跨从站回环", "canloop"}, {"probe · CSV 实时性探测", "probe"},
            {"reset · 复位从站", "reset"}};
        for (const auto &command : commands)
            advancedCommand_->addItem(command.first, command.second);
        advancedArgs_ = new QLineEdit(card);
        advancedArgs_->setPlaceholderText(QStringLiteral("例如：3000 或 2 3 30 1000 0"));
        form->addRow(QStringLiteral("主站命令"), advancedCommand_);
        form->addRow(QStringLiteral("参数"), advancedArgs_);
        advancedHelp_ = new QLabel(card);
        advancedHelp_->setObjectName("muted");
        advancedHelp_->setWordWrap(true);
        auto *run = makeButton(QStringLiteral("运行命令"), card, "primary");
        cardLayout->addWidget(title);
        cardLayout->addLayout(form);
        cardLayout->addWidget(advancedHelp_);
        cardLayout->addWidget(run, 0, Qt::AlignLeft);
        layout->addWidget(card);

        auto *note = new QLabel(QStringLiteral("参数以空格分隔并直接传给主站，不经过 shell。命令详细用法可在终端运行 build/ecat2can_master --help。probe 还需要预先设置 ECAT2CAN_PROBE_CSV 环境变量。"), page);
        note->setObjectName("muted");
        note->setWordWrap(true);
        layout->addWidget(note);
        layout->addStretch();
        connect(advancedCommand_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this](int) { updateAdvancedHelp(); });
        connect(run, &QPushButton::clicked, this, [this] {
            QStringList args{advancedCommand_->currentData().toString()};
            const QString raw = advancedArgs_->text().trimmed();
            if (!raw.isEmpty())
                args.append(raw.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts));
            const QString command = advancedCommand_->currentData().toString();
            const QSet<QString> transmitting{
                "work", "workall", "bench", "bench_lat", "looplat", "looplat2", "canloop", "probe"};
            if (transmitting.contains(command)) {
                confirmAndRun(QStringLiteral("确认运行主站命令"),
                              QStringLiteral("命令 %1 可能向 CAN 总线发送测试帧。请确认接线和设备状态。")
                                  .arg(command), args);
            } else {
                runMaster(args);
            }
        });
        updateAdvancedHelp();
        return scrollPage(page, parent);
    }

    QWidget *scrollPage(QWidget *content, QWidget *parent)
    {
        auto *scroll = new QScrollArea(parent);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidget(content);
        return scroll;
    }

    void updateAdvancedHelp()
    {
        const QString command = advancedCommand_->currentData().toString();
        const QHash<QString, QString> usage{
            {"regs", "读取 ESC 的 DL_STAT 和 STATION_ADDR。"},
            {"info", "持续监视从站状态；停止任务可退出。"},
            {"loop", "可选参数：PDO 往返轮数，默认 1000。"},
            {"scan", "可选参数：扫描轮数，默认 3000，只读不发。"},
            {"work", "参数：通道 ID [数据字节…] [--hz=频率] [--frames=帧数]。"},
            {"workall", "参数：ID [数据字节…] [--hz=频率] [--frames=帧数]。"},
            {"fdb", "可选参数：读取轮数，默认 20。"},
            {"bench", "参数：[测试轮数] [通道]；会产生测试流量。"},
            {"rt", "参数：[秒数] [通道|host]；host 模式无需从站。"},
            {"bench_lat", "参数：通道 CAN接口 [测试轮数]，例如 3 can0 3000。"},
            {"looplat", "参数：发送通道 接收通道 [秒数] [Hz] [负载帧数]。"},
            {"looplat2", "参数：从站列表 发送通道 接收通道 [秒数] [Hz] [负载帧数]。"},
            {"canloop", "参数：发送从站 发送通道 接收从站 接收通道 [秒数] [Hz]。"},
            {"probe", "参数：秒数 发送通道|ecat 接收通道 [Hz] [负载帧数]。"},
            {"reset", "触发从站硬件复位。"}};
        advancedHelp_->setText(usage.value(command));
        if (advancedArgs_->text().isEmpty()) {
            const QHash<QString, QString> defaults{
                {"loop", "1000"}, {"scan", "3000"}, {"fdb", "20"},
                {"bench", "5000 3"}, {"rt", "10 host"}, {"looplat", "2 3 30 1000 0"}};
            advancedArgs_->setPlaceholderText(defaults.value(command, QStringLiteral("按需填写参数")));
        }
    }

    bool parseCanFrame(QLineEdit *idEdit, QLineEdit *dataEdit, QStringList &args)
    {
        bool idOk = false;
        const uint id = idEdit->text().toUInt(&idOk, 0);
        if (!idOk || id > 0x7ff) {
            QMessageBox::warning(this, QStringLiteral("报文格式错误"),
                                 QStringLiteral("标准 CAN ID 必须是 0 到 0x7FF 之间的整数。"));
            return false;
        }
        const QStringList bytes = dataEdit->text().split(
            QRegularExpression(QStringLiteral("[ ,;]+")), Qt::SkipEmptyParts);
        if (bytes.isEmpty() || bytes.size() > 8) {
            QMessageBox::warning(this, QStringLiteral("报文格式错误"),
                                 QStringLiteral("数据长度必须为 1 到 8 个十六进制字节。"));
            return false;
        }
        args << QStringLiteral("0x%1").arg(id, 0, 16);
        for (const QString &byte : bytes) {
            bool ok = false;
            const uint value = byte.toUInt(&ok, 16);
            if (!ok || value > 0xff) {
                QMessageBox::warning(this, QStringLiteral("报文格式错误"),
                                     QStringLiteral("数据字节需使用 00 到 FF 之间的十六进制数。"));
                args.clear();
                return false;
            }
            args << QStringLiteral("%1").arg(value, 2, 16, QLatin1Char('0'));
        }
        return true;
    }

    void confirmAndRun(const QString &title, const QString &message,
                       const QStringList &arguments)
    {
        const auto answer = QMessageBox::warning(
            this, title, message, QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer == QMessageBox::Yes) runMaster(arguments);
    }

    void refreshInterfaces()
    {
        const QString previous = selectedInterface();
        interfaceBox_->clear();
        for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
            if (iface.name().isEmpty()) continue;
            const bool up = iface.flags().testFlag(QNetworkInterface::IsUp);
            interfaceBox_->addItem(QStringLiteral("%1  ·  %2").arg(
                                       iface.name(), up ? QStringLiteral("已启用") : QStringLiteral("未启用")),
                                   iface.name());
        }
        const int index = interfaceBox_->findData(previous);
        if (index >= 0) interfaceBox_->setCurrentIndex(index);
        if (interfaceBox_->count() == 0) interfaceBox_->addItem(QStringLiteral("未发现网卡"), QString());
        updateInterfaceHint();
    }

    void updateInterfaceHint()
    {
        const QString name = selectedInterface();
        const QNetworkInterface iface = QNetworkInterface::interfaceFromName(name);
        if (name.isEmpty() || !iface.isValid()) {
            interfaceHint_->setText(QStringLiteral("未检测到网卡"));
            return;
        }
        interfaceHint_->setText(QStringLiteral("%1 · %2")
                                    .arg(iface.flags().testFlag(QNetworkInterface::IsRunning)
                                             ? QStringLiteral("链路运行") : QStringLiteral("无链路"),
                                         iface.hardwareAddress()));
    }

    void runMaster(const QStringList &arguments, bool requireInterface = true)
    {
        if (isRunning()) {
            QMessageBox::information(this, QStringLiteral("任务正在运行"),
                                     QStringLiteral("请先停止当前任务，再启动另一项测试。"));
            return;
        }
        QString iface = selectedInterface();
        if (iface.isEmpty() && !requireInterface) iface = QStringLiteral("lo");
        if (iface.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("缺少网卡"),
                                 QStringLiteral("请先选择 EtherCAT 网卡。纯主机实时性测试不需要网卡。"));
            return;
        }
        const QString executable = QDir(QApplication::applicationDirPath()).filePath("ecat2can_master");
        if (!QFileInfo::exists(executable)) {
            QMessageBox::critical(this, QStringLiteral("找不到主站程序"),
                                  QStringLiteral("未找到同目录下的 ecat2can_master：\n%1").arg(executable));
            return;
        }

        QStringList args{iface};
        args.append(arguments);
        startProcess(executable, args, QStringLiteral("ecat2can_master"));
    }

    void startProcess(const QString &executable, const QStringList &args, const QString &name)
    {
        processBuffer_.clear();
        output_->appendPlainText(QStringLiteral("\n$ %1 %2").arg(name, args.join(' ')));
        status_->setText(QStringLiteral("运行中"));
        status_->setProperty("busy", true);
        status_->setProperty("error", false);
        status_->style()->unpolish(status_);
        status_->style()->polish(status_);
        stopButton_->setEnabled(true);
        process_->setProgram(executable);
        process_->setArguments(args);
        process_->start();
    }

    void stopProcess()
    {
        if (!isRunning()) return;
        output_->appendPlainText(QStringLiteral("\n正在请求任务安全停止…"));
        process_->terminate();
        killTimer_->start(3000);
    }

    void connectProcess()
    {
        connect(killTimer_, &QTimer::timeout, this, [this] {
            if (isRunning()) process_->kill();
        });
        connect(process_, &QProcess::readyReadStandardOutput, this, [this] {
            processBuffer_ += QString::fromLocal8Bit(process_->readAllStandardOutput());
            int newline = -1;
            while ((newline = processBuffer_.indexOf(QLatin1Char('\n'))) >= 0) {
                const QString line = processBuffer_.left(newline).trimmed();
                processBuffer_.remove(0, newline + 1);
                if (line.startsWith(QStringLiteral("MOTOR_CSV,"))) {
                    const QStringList f = line.mid(10).split(QLatin1Char(','));
                    if (f.size() >= 9) {
                        bool ok[6]{};
                        const double pos = f[1].toDouble(&ok[0]);
                        const double target = f[2].toDouble(&ok[1]);
                        const double rpm = f[3].toDouble(&ok[2]);
                        const double rpmTarget = f[4].toDouble(&ok[3]);
                        const double current = f[5].toDouble(&ok[4]);
                        const double output = f[6].toDouble(&ok[5]);
                        if (motorPlot_ && std::all_of(std::begin(ok), std::end(ok), [](bool v) { return v; }))
                            motorPlot_->addSample(pos, target, rpm, rpmTarget, current, output);
                    }
                } else if (!line.isEmpty()) {
                    output_->appendPlainText(line);
                }
            }
        });
        connect(process_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
                [this](int code, QProcess::ExitStatus exitStatus) {
            killTimer_->stop();
            status_->setProperty("busy", false);
            status_->setProperty("error", code != 0 || exitStatus != QProcess::NormalExit);
            status_->setText(code == 0 && exitStatus == QProcess::NormalExit
                                 ? QStringLiteral("已完成") : QStringLiteral("异常退出"));
            status_->style()->unpolish(status_);
            status_->style()->polish(status_);
            stopButton_->setEnabled(false);
            motorProcess_ = false;
            if (!processBuffer_.isEmpty()) { output_->appendPlainText(processBuffer_); processBuffer_.clear(); }
            output_->appendPlainText(QStringLiteral("\n任务结束，退出码 %1。若提示权限不足，请按使用指南配置 CAP_NET_RAW。")
                                         .arg(code));
        });
        connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error != QProcess::FailedToStart) return;
            killTimer_->stop();
            status_->setText(QStringLiteral("启动失败"));
            status_->setProperty("busy", false);
            status_->setProperty("error", true);
            status_->style()->unpolish(status_);
            status_->style()->polish(status_);
            stopButton_->setEnabled(false);
            motorProcess_ = false;
            output_->appendPlainText(QStringLiteral("无法启动主站程序：%1").arg(process_->errorString()));
        });
    }

    void closeEvent(QCloseEvent *event) override
    {
        if (isRunning()) {
            process_->terminate();
            if (!process_->waitForFinished(2500)) process_->kill();
        }
        QMainWindow::closeEvent(event);
    }
};

} // namespace

int main(int argc, char *argv[])
{
    // Wayland 会话缺少 Qt Wayland 插件时，存在 XWayland 则退回 xcb。
    const QByteArray requestedPlatform = qgetenv("QT_QPA_PLATFORM");
    if (requestedPlatform.startsWith("wayland")) {
        const QString platformDir = QDir(QLibraryInfo::location(QLibraryInfo::PluginsPath))
                                        .filePath(QStringLiteral("platforms"));
        const QStringList plugins = QDir(platformDir).entryList(
            {QStringLiteral("libqwayland*.so")}, QDir::Files | QDir::Readable);
        if (plugins.isEmpty() && !qEnvironmentVariableIsEmpty("DISPLAY")) {
            qWarning() << "Qt Wayland plugin unavailable, falling back to xcb.";
            qputenv("QT_QPA_PLATFORM", "xcb");
        }
    }
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("ECAT2CAN 测试工作台"));
    QApplication::setOrganizationName(QStringLiteral("Master_demo"));
    MasterWindow window;
    window.show();
    return app.exec();
}
