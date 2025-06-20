/****************************************************************************
**
** This file is part of the KD Soap project.
**
** SPDX-FileCopyrightText: 2010 Klarälvdalens Datakonsult AB, a KDAB Group company <info@kdab.com>
**
** SPDX-License-Identifier: MIT
**
****************************************************************************/
#include "KDSoapValue.h"
#include "KDDateTime.h"
#include "KDSoapNamespaceManager.h"
#include "KDSoapNamespacePrefixes_p.h"
#include <QDateTime>
#include <QDebug>
#include <QStringList>
#include <QUrl>

class KDSoapValue::Private : public QSharedData
{
public:
    Private()
        : m_qualified(false)
        , m_nillable(false)
    {
    }
    Private(const QString &n, const QVariant &v, const QString &typeNameSpace, const QString &typeName)
        : m_name(n)
        , m_value(v)
        , m_typeNamespace(typeNameSpace)
        , m_typeName(typeName)
        , m_qualified(false)
        , m_nillable(false)
    {
    }

    QString m_name;
    QString m_nameNamespace;
    QVariant m_value;
    QString m_typeNamespace;
    QString m_typeName;
    KDSoapValueList m_childValues;
    bool m_qualified;
    bool m_nillable;
    QXmlStreamNamespaceDeclarations m_environmentNamespaceDeclarations;
    QXmlStreamNamespaceDeclarations m_localNamespaceDeclarations;
};

uint qHash(const KDSoapValue &value)
{
    return qHash(value.name());
}

KDSoapValue::KDSoapValue()
    : d(new Private)
{
}

KDSoapValue::KDSoapValue(const QString &n, const QVariant &v, const QString &typeNameSpace, const QString &typeName)
    : d(new Private(n, v, typeNameSpace, typeName))
{
}

KDSoapValue::KDSoapValue(const QString &n, const KDSoapValueList &children, const QString &typeNameSpace, const QString &typeName)
    : d(new Private(n, QVariant(), typeNameSpace, typeName))
{
    d->m_childValues = children;
}

KDSoapValue::~KDSoapValue()
{
}

KDSoapValue::KDSoapValue(const KDSoapValue &other)
    : d(other.d)
{
}

bool KDSoapValue::isNull() const
{
    return d->m_name.isEmpty() && isNil();
}

bool KDSoapValue::isNil() const
{
    return d->m_value.isNull() && d->m_childValues.isEmpty() && d->m_childValues.attributes().isEmpty();
}

void KDSoapValue::setNillable(bool nillable)
{
    d->m_nillable = nillable;
}

QString KDSoapValue::name() const
{
    return d->m_name;
}

void KDSoapValue::setName(const QString &name)
{
    d->m_name = name;
}

QVariant KDSoapValue::value() const
{
    return d->m_value;
}

void KDSoapValue::setValue(const QVariant &value)
{
    d->m_value = value;
}

bool KDSoapValue::isQualified() const
{
    return d->m_qualified;
}

void KDSoapValue::setQualified(bool qualified)
{
    d->m_qualified = qualified;
}

void KDSoapValue::setNamespaceDeclarations(const QXmlStreamNamespaceDeclarations &namespaceDeclarations)
{
    d->m_localNamespaceDeclarations = namespaceDeclarations;
}

void KDSoapValue::addNamespaceDeclaration(const QXmlStreamNamespaceDeclaration &namespaceDeclaration)
{
    d->m_localNamespaceDeclarations.append(namespaceDeclaration);
}

QXmlStreamNamespaceDeclarations KDSoapValue::namespaceDeclarations() const
{
    return d->m_localNamespaceDeclarations;
}

void KDSoapValue::setEnvironmentNamespaceDeclarations(const QXmlStreamNamespaceDeclarations &environmentNamespaceDeclarations)
{
    d->m_environmentNamespaceDeclarations = environmentNamespaceDeclarations;
}

QXmlStreamNamespaceDeclarations KDSoapValue::environmentNamespaceDeclarations() const
{
    return d->m_environmentNamespaceDeclarations;
}

KDSoapValueList &KDSoapValue::childValues() const
{
    // I want to fool the QSharedDataPointer mechanism here...
    return const_cast<KDSoapValueList &>(d->m_childValues);
}

bool KDSoapValue::operator==(const KDSoapValue &other) const
{
    return d == other.d;
}

bool KDSoapValue::operator!=(const KDSoapValue &other) const
{
    return d != other.d;
}

static QString variantToTextValue(const QVariant &value, const QString &typeNs, const QString &type)
{
    switch (value.userType()) {
    case QMetaType::QChar:
    // fall-through
    case QMetaType::QString:
        return value.toString();
    case QMetaType::QUrl:
        // xmlpatterns/data/qatomicvalue.cpp says to do this:
        return value.toUrl().toString();
    case QMetaType::QByteArray: {
        const QByteArray data = value.toByteArray();
        if (typeNs == KDSoapNamespaceManager::xmlSchema1999() || typeNs == KDSoapNamespaceManager::xmlSchema2001()) {
            if (type == QLatin1String("hexBinary")) {
                const QByteArray hb = data.toHex();
                return QString::fromLatin1(hb.constData(), hb.size());
            }
        }
        // default to base64Binary, like variantToXMLType() does.
        const QByteArray b64 = value.toByteArray().toBase64();
        return QString::fromLatin1(b64.constData(), b64.size());
    }
    case QMetaType::Int:
    // fall-through
    case QMetaType::LongLong:
    // fall-through
    case QMetaType::UInt:
        return QString::number(value.toLongLong());
    case QMetaType::ULongLong:
        return QString::number(value.toULongLong());
    case QMetaType::Bool:
    case QMetaType::Float:
    case QMetaType::Double:
        return value.toString();
    case QMetaType::QTime: {
        const QTime time = value.toTime();
        if (time.msec()) {
            // include milli-seconds
            return time.toString(QLatin1String("hh:mm:ss.zzz"));
        } else {
            return time.toString(Qt::ISODate);
        }
    }
    case QMetaType::QDate:
        return value.toDate().toString(Qt::ISODate);
    case QMetaType::QDateTime: // https://www.w3.org/TR/xmlschema-2/#dateTime
        return KDDateTime(value.toDateTime()).toDateString();
    case QMetaType::UnknownType:
        qDebug() << "ERROR: Got invalid QVariant in a KDSoapValue";
        return QString();
    default:
        if (value.canConvert<KDDateTime>()) {
            return value.value<KDDateTime>().toDateString();
        }

        if (value.userType() == qMetaTypeId<float>()) {
            return QString::number(value.value<float>());
        }

        qDebug() << QString::fromLatin1("QVariants of type %1 are not supported in "
                                        "KDSoap, see the documentation")
                        .arg(QLatin1String(value.typeName()));
        return value.toString();
    }
}

// See also xmlTypeToVariant in serverlib
static QString variantToXMLType(const QVariant &value)
{
    switch (value.userType()) {
    case QMetaType::QChar:
    // fall-through
    case QMetaType::QString:
    // fall-through
    case QMetaType::QUrl:
        return QLatin1String("xsd:string");
    case QMetaType::QByteArray:
        return QLatin1String("xsd:base64Binary");
    case QMetaType::Int:
    // fall-through
    case QMetaType::LongLong:
    // fall-through
    case QMetaType::UInt:
        return QLatin1String("xsd:int");
    case QMetaType::ULongLong:
        return QLatin1String("xsd:unsignedInt");
    case QMetaType::Bool:
        return QLatin1String("xsd:boolean");
    case QMetaType::Float:
        return QLatin1String("xsd:float");
    case QMetaType::Double:
        return QLatin1String("xsd:double");
    case QMetaType::QTime:
        return QLatin1String("xsd:time"); // correct? xmlpatterns fallsback to datetime because of missing timezone
    case QMetaType::QDate:
        return QLatin1String("xsd:date");
    case QMetaType::QDateTime:
        return QLatin1String("xsd:dateTime");
    default:
        if (value.userType() == qMetaTypeId<float>()) {
            return QLatin1String("xsd:float");
        }
        if (value.canConvert<KDDateTime>()) {
            return QLatin1String("xsd:dateTime");
        }

        qDebug() << value;

        qDebug() << QString::fromLatin1("variantToXmlType: QVariants of type %1 are not supported in "
                                        "KDSoap, see the documentation")
                        .arg(QLatin1String(value.typeName()));
        return QString();
    }
}

void KDSoapValue::writeElement(KDSoapNamespacePrefixes &namespacePrefixes, QXmlStreamWriter &writer, KDSoapValue::Use use,
                               const QString &messageNamespace, bool forceQualified) const
{
    Q_ASSERT(!name().isEmpty());
    if (!d->m_nameNamespace.isEmpty() && d->m_nameNamespace != messageNamespace) {
        forceQualified = true;
    }

    if (d->m_qualified || forceQualified) {
        const QString ns = d->m_nameNamespace.isEmpty() ? messageNamespace : d->m_nameNamespace;

        // TODO: if the prefix is new, we want to do namespacePrefixes.insert()
        // But this means figuring out n2/n3/n4 the same way Qt does...

        writer.writeStartElement(ns, name());
    } else {
        writer.writeStartElement(name());
    }
    writeElementContents(namespacePrefixes, writer, use, messageNamespace);
    writer.writeEndElement();
}

void KDSoapValue::writeElementContents(KDSoapNamespacePrefixes &namespacePrefixes, QXmlStreamWriter &writer, KDSoapValue::Use use,
                                       const QString &messageNamespace) const
{
    const QVariant value = this->value();

    for (const QXmlStreamNamespaceDeclaration &decl : std::as_const(d->m_localNamespaceDeclarations)) {
        writer.writeNamespace(decl.namespaceUri().toString(), decl.prefix().toString());
    }

    if (isNil() && d->m_nillable) {
        writer.writeAttribute(KDSoapNamespaceManager::xmlSchemaInstance2001(), QLatin1String("nil"), QLatin1String("true"));
    }

    if (use == EncodedUse) {
        // use=encoded means writing out xsi:type attributes
        QString type;
        if (!this->type().isEmpty()) {
            type = namespacePrefixes.resolve(this->typeNs(), this->type());
        }
        if (type.isEmpty() && !value.isNull()) {
            type = variantToXMLType(value); // fallback
        }
        if (!type.isEmpty()) {
            writer.writeAttribute(KDSoapNamespaceManager::xmlSchemaInstance2001(), QLatin1String("type"), type);
        }

        const KDSoapValueList list = this->childValues();
        const bool isArray = !list.arrayType().isEmpty();
        if (isArray) {
            writer.writeAttribute(KDSoapNamespaceManager::soapEncoding(), QLatin1String("arrayType"),
                                  namespacePrefixes.resolve(list.arrayTypeNs(), list.arrayType()) + QLatin1Char('[') + QString::number(list.count())
                                      + QLatin1Char(']'));
        }
    }
    writeChildren(namespacePrefixes, writer, use, messageNamespace, false);

    if (!value.isNull()) {
        const QString txt = variantToTextValue(value, this->typeNs(), this->type());
        if (!txt.isEmpty()) { // In Qt6, a null string doesn't lead to a null variant anymore
            writer.writeCharacters(txt);
        }
    }
}

void KDSoapValue::writeChildren(KDSoapNamespacePrefixes &namespacePrefixes, QXmlStreamWriter &writer, KDSoapValue::Use use,
                                const QString &messageNamespace, bool forceQualified) const
{
    const KDSoapValueList &args = childValues();
    const auto &attributes = args.attributes();
    for (const KDSoapValue &attr : attributes) {
        // Q_ASSERT(!attr.value().isNull());

        const QString attributeNamespace = attr.namespaceUri();
        if (attr.isQualified() || forceQualified) {
            writer.writeAttribute(attributeNamespace, attr.name(), variantToTextValue(attr.value(), attr.typeNs(), attr.type()));
        } else {
            writer.writeAttribute(attr.name(), variantToTextValue(attr.value(), attr.typeNs(), attr.type()));
        }
    }
    KDSoapValueListIterator it(args);
    while (it.hasNext()) {
        const KDSoapValue &element = it.next();
        element.writeElement(namespacePrefixes, writer, use, messageNamespace, forceQualified);
    }
}

////

QDebug operator<<(QDebug dbg, const KDSoapValue &value)
{
    dbg.space() << value.name() << value.value();
    if (!value.childValues().isEmpty()) {
        dbg << "<children>";
        KDSoapValueListIterator it(value.childValues());
        while (it.hasNext()) {
            const KDSoapValue &child = it.next();
            dbg << child;
        }
        dbg << "</children>";
    }
    if (!value.childValues().attributes().isEmpty()) {
        dbg << "<attributes>";
        QListIterator<KDSoapValue> it(value.childValues().attributes());
        while (it.hasNext()) {
            const KDSoapValue &child = it.next();
            dbg << child;
        }
        dbg << "</attributes>";
    }
    return dbg;
}

void KDSoapValue::setType(const QString &nameSpace, const QString &type)
{
    d->m_typeNamespace = nameSpace;
    d->m_typeName = type;
}

QString KDSoapValue::typeNs() const
{
    return d->m_typeNamespace;
}

QString KDSoapValue::type() const
{
    return d->m_typeName;
}

KDSoapValueList KDSoapValue::split() const
{
    KDSoapValueList valueList;
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    const QStringList list = value().toString().split(QLatin1Char(' '), Qt::SkipEmptyParts);
#else
    const QStringList list = value().toString().split(QLatin1Char(' '), QString::SkipEmptyParts);
#endif
    valueList.reserve(list.count());
    for (const QString &part : std::as_const(list)) {
        KDSoapValue value(*this);
        value.setValue(part);
        valueList << value;
    }
    return valueList;
}

KDSoapValue KDSoapValueList::child(const QString &name) const
{
    for (const KDSoapValue &val : std::as_const(*this)) {
        if (val.name() == name) {
            return val;
        }
    }
    return KDSoapValue();
}

void KDSoapValueList::setArrayType(const QString &nameSpace, const QString &type)
{
    m_arrayType = qMakePair(nameSpace, type);
}

QString KDSoapValueList::arrayTypeNs() const
{
    return m_arrayType.first;
}

QString KDSoapValueList::arrayType() const
{
    return m_arrayType.second;
}

void KDSoapValueList::addArgument(const QString &argumentName, const QVariant &argumentValue, const QString &typeNameSpace, const QString &typeName)
{
    append(KDSoapValue(argumentName, argumentValue, typeNameSpace, typeName));
}

QString KDSoapValue::namespaceUri() const
{
    return d->m_nameNamespace;
}

void KDSoapValue::setNamespaceUri(const QString &ns)
{
    d->m_nameNamespace = ns;
}

QByteArray KDSoapValue::toXml(KDSoapValue::Use use, const QString &messageNamespace) const
{
    QByteArray data;
    QXmlStreamWriter writer(&data);
    writer.writeStartDocument();

    KDSoapNamespacePrefixes namespacePrefixes;
    namespacePrefixes.writeStandardNamespaces(writer);

    writeElement(namespacePrefixes, writer, use, messageNamespace, false);
    writer.writeEndDocument();

    return data;
}
