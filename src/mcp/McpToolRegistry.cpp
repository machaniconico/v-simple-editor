#include "McpToolRegistry.h"

#include <exception>

namespace mcp {

void McpToolRegistry::registerTool(const ToolDescriptor& tool)
{
    for (ToolDescriptor& registeredTool : m_tools) {
        if (registeredTool.name == tool.name) {
            registeredTool = tool;
            return;
        }
    }

    m_tools.append(tool);
}

void McpToolRegistry::clear()
{
    m_tools.clear();
}

bool McpToolRegistry::contains(const QString& name) const
{
    for (const ToolDescriptor& tool : m_tools) {
        if (tool.name == name)
            return true;
    }

    return false;
}

QVector<QString> McpToolRegistry::toolNames() const
{
    QVector<QString> names;
    names.reserve(m_tools.size());
    for (const ToolDescriptor& tool : m_tools)
        names.append(tool.name);
    return names;
}

QJsonArray McpToolRegistry::listToolsJson() const
{
    QJsonArray tools;
    for (const ToolDescriptor& tool : m_tools) {
        QJsonObject descriptor;
        descriptor.insert(QStringLiteral("name"), tool.name);
        descriptor.insert(QStringLiteral("description"), tool.description);
        descriptor.insert(QStringLiteral("inputSchema"), tool.inputSchema);
        if (!tool.outputSchema.isEmpty())
            descriptor.insert(QStringLiteral("outputSchema"), tool.outputSchema);
        tools.append(descriptor);
    }

    return tools;
}

QJsonObject McpToolRegistry::callTool(const QString& name, const QJsonObject& args,
                                      bool* found, QString* err,
                                      QJsonArray* content) const
{
    if (found)
        *found = false;
    if (err)
        err->clear();
    if (content)
        *content = QJsonArray();

    for (const ToolDescriptor& tool : m_tools) {
        if (tool.name != name)
            continue;

        if (found)
            *found = true;

        QString localError;
        QString* handlerError = err ? err : &localError;
        if (!tool.handler && !tool.contentHandler) {
            *handlerError = QStringLiteral("tool handler is not set");
            return QJsonObject();
        }

        try {
            if (tool.contentHandler)
                return tool.contentHandler(args, handlerError, content);
            return tool.handler(args, handlerError);
        } catch (const std::exception& exception) {
            if (content)
                *content = QJsonArray();
            *handlerError = QStringLiteral("tool handler exception: %1")
                .arg(QString::fromUtf8(exception.what()));
        } catch (...) {
            if (content)
                *content = QJsonArray();
            *handlerError = QStringLiteral("tool handler exception");
        }

        return QJsonObject();
    }

    return QJsonObject();
}

} // namespace mcp
