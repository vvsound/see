#pragma once
#include <QString>
#include <QStringList>
#include <QColor>
#include <QList>
#include <QMap>

// 单个配置项的类型说明(来自模板里的 ;@ 行)
struct ConfigKeyMeta {
    QString key;            // 完整键名,如 "Browse/ShowToolbar"
    QString type;           // bool / int / double / enum / string / color / list
    QString defaultValue;   // 模板中的默认值
    QString comment;        // 中文注释
    QStringList enumValues; // type==enum 时的可选值
    double minValue;        // int/double 的最小值
    double maxValue;        // int/double 的最大值
    bool hasRange;          // 是否有范围
};

// 读写 QuickView.ini(保留用户注释,修改时只改那一行;缺项/非法值直接报错退出)
class Config {
public:
    // 全局唯一实例
    static Config& instance();
    // exe 所在文件夹(末尾无斜杠)
    static QString appDir();
    // QuickView.ini 的完整路径
    static QString iniPath();
    // 显示错误对话框并立刻退出程序(不兜底)
    static void fatal(const QString& message);
    // 命令行小工具模式:fatal 不弹对话框,改为把错误写到标准错误后退出(退出码 1)
    static void setCliMode(bool on);
    // 某个键在用户 ini 里的行号(1 起);不存在返回 0
    int lineNumber(const QString& key) const;

    // 首次运行:ini 不存在则用内置模板生成
    bool ensureFileExists(QString* error);
    // 读取 ini 并对照模板逐项检查(缺项、类型错、超范围都会失败,error 里写明行号)
    bool load(QString* error);

    // 读取配置;key 形如 "Browse/ShowToolbar";缺项/非法立刻 fatal
    QString getString(const QString& key) const;
    bool getBool(const QString& key) const;
    int getInt(const QString& key) const;
    double getDouble(const QString& key) const;
    QColor getColor(const QString& key) const;
    // 逗号分隔的列表(去掉空白和空项)
    QStringList getList(const QString& key) const;
    // 枚举值(必须是模板声明的可选值之一)
    QString getEnum(const QString& key) const;

    // 修改配置并立刻写回文件,只改对应那一行
    void setString(const QString& key, const QString& value);
    void setBool(const QString& key, bool value);
    void setInt(const QString& key, int value);
    void setDouble(const QString& key, double value);
    void setList(const QString& key, const QStringList& value);

    // 某个键的类型说明(找不到返回 nullptr)
    const ConfigKeyMeta* meta(const QString& key) const;
    // 模板里所有键(按模板顺序)
    QStringList allKeys() const;
    // 某个段里的所有键名(不含段名)
    QStringList keysInSection(const QString& section) const;

private:
    Config();
    struct Line {
        QString raw;      // 原始整行文字
        QString section;  // 所在段
        QString key;      // 键名(仅 isEntry 时有效)
        QString value;    // 值
        bool isEntry;     // 是否 key=value 行
    };
    // 解析文本为行列表;遇到不是 项目=值 的行返回 false 并写 error(含行号)
    bool parseText(const QString& text, const QString& sourceName, QList<Line>* out, QString* error) const;
    // 解析内置模板,填充 metas_
    bool loadTemplate(QString* error);
    // 校验单个值是否符合类型;不符合返回 false 并写明原因
    bool validateValue(const ConfigKeyMeta& m, const QString& value, QString* why) const;
    // 取原始值(必须存在,否则 fatal)
    const Line& lineFor(const QString& key) const;
    // 写回文件
    void writeFile();

    QList<Line> lines_;                  // 用户 ini 的全部行
    QMap<QString, int> index_;           // 键名 -> lines_ 下标
    QList<ConfigKeyMeta> metas_;         // 模板里的所有键(有序)
    QMap<QString, int> metaIndex_;       // 键名 -> metas_ 下标
    QString templateText_;               // 内置模板全文
    bool loaded_;
};
