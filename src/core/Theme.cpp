#include "Theme.h"
#include "Config.h"

#include <QApplication>
#include <QFile>
#include <QHash>
#include <QRegularExpression>

// theme\current.qss 的完整路径
QString Theme::path()
{
    return Config::appDir() + QStringLiteral("/theme/current.qss");
}

// 读取样式表文本
bool Theme::read(QString* qss, QString* error)
{
    QFile f(path());
    if (!f.exists()) {
        *error = QStringLiteral("找不到主题文件 %1。请用 MoonManager 的“主题”页应用一个主题,再“部署”。").arg(path());
        return false;
    }
    if (!f.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("无法读取主题文件 %1:%2").arg(path()).arg(f.errorString());
        return false;
    }
    *qss = QString::fromUtf8(f.readAll());
    return true;
}

namespace {
QHash<QString, QString>& varTable()
{
    static QHash<QString, QString> t;
    return t;
}

// 把 qss 里的 /*@var 名字=值*/ 解析成表
QHash<QString, QString> parseVars(const QString& qss)
{
    QHash<QString, QString> out;
    static const QRegularExpression re(QStringLiteral("/\\*@var\\s+([A-Za-z_][A-Za-z0-9_]*)\\s*=\\s*([^*]*?)\\s*\\*/"));
    QRegularExpressionMatchIterator it = re.globalMatch(qss);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        out.insert(m.captured(1), m.captured(2));
    }
    return out;
}
} // namespace

QStringList Theme::requiredVars()
{
    return QStringList() << QStringLiteral("bg") << QStringLiteral("accent") << QStringLiteral("accent_soft") << QStringLiteral("accent_fg")
                         << QStringLiteral("hover") << QStringLiteral("focus") << QStringLiteral("fg")
                         << QStringLiteral("fg_dim") << QStringLiteral("border") << QStringLiteral("base")
                         << QStringLiteral("panel") << QStringLiteral("danger") << QStringLiteral("icon");
}

bool Theme::checkVars(const QString& qss, QString* error)
{
    const QHash<QString, QString> t = parseVars(qss);
    const QStringList need = requiredVars();
    for (int i = 0; i < need.size(); ++i) {
        if (!t.contains(need.at(i))) {
            *error = QStringLiteral("主题文件 %1 里没有程序需要的主题变量 %2(应有一行 /*@var %2=颜色*/)。请用 MoonManager 的“主题”页重新应用主题;若改过 template.qss,请把这一行补回去。")
                         .arg(path()).arg(need.at(i));
            return false;
        }
        if (!QColor(t.value(need.at(i))).isValid()) {
            *error = QStringLiteral("主题文件 %1 里主题变量 %2 的值“%3”不是合法颜色(要写成 #RRGGBB)。").arg(path()).arg(need.at(i)).arg(t.value(need.at(i)));
            return false;
        }
    }
    return true;
}

QString Theme::var(const QString& name)
{
    const QHash<QString, QString>& t = varTable();
    if (!t.contains(name)) Config::fatal(QStringLiteral("主题里没有变量 %1(文件 %2)。").arg(name).arg(path()));
    return t.value(name);
}

QColor Theme::color(const QString& name)
{
    const QColor c(var(name));
    if (!c.isValid()) Config::fatal(QStringLiteral("主题变量 %1 的值“%2”不是合法颜色(文件 %3)。").arg(name).arg(var(name)).arg(path()));
    return c;
}

// 读取并应用
bool Theme::apply(QString* error)
{
    QString qss;
    if (!read(&qss, error)) return false;
    if (!checkVars(qss, error)) return false;
    varTable() = parseVars(qss);
    qApp->setStyleSheet(qss);
    return true;
}
