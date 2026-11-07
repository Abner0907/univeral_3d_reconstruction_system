#include "PlyLoader.h"

#include <QFile>
#include <QList>
#include <QtEndian>

#include <cmath>
#include <cstring>

namespace u3d {

namespace {

constexpr float kShC0 = 0.28209479177387814f;

enum class Type { Int8, UInt8, Int16, UInt16, Int32, UInt32, Float32, Float64, Invalid };
enum class Format { Ascii, BinaryLittleEndian, BinaryBigEndian };

Type parseType(const QByteArray &t)
{
    if (t == "char" || t == "int8") return Type::Int8;
    if (t == "uchar" || t == "uint8") return Type::UInt8;
    if (t == "short" || t == "int16") return Type::Int16;
    if (t == "ushort" || t == "uint16") return Type::UInt16;
    if (t == "int" || t == "int32") return Type::Int32;
    if (t == "uint" || t == "uint32") return Type::UInt32;
    if (t == "float" || t == "float32") return Type::Float32;
    if (t == "double" || t == "float64") return Type::Float64;
    return Type::Invalid;
}

int typeSize(Type t)
{
    switch (t) {
    case Type::Int8: case Type::UInt8: return 1;
    case Type::Int16: case Type::UInt16: return 2;
    case Type::Int32: case Type::UInt32: case Type::Float32: return 4;
    case Type::Float64: return 8;
    case Type::Invalid: break;
    }
    return 0;
}

struct Property {
    QByteArray name;
    Type type = Type::Invalid;
    bool isList = false;
    Type countType = Type::Invalid;
};

struct Element {
    QByteArray name;
    qint64 count = 0;
    std::vector<Property> props;
};

class BodyReader
{
public:
    BodyReader(const char *begin, const char *end, Format format)
        : m_p(begin), m_end(end), m_format(format) {}

    bool read(Type type, double &value)
    {
        if (m_format == Format::Ascii)
            return readAscii(value);
        const int size = typeSize(type);
        if (m_end - m_p < size)
            return false;
        const bool be = m_format == Format::BinaryBigEndian;
        const uchar *p = reinterpret_cast<const uchar *>(m_p);
        switch (type) {
        case Type::Int8: value = double(qint8(*p)); break;
        case Type::UInt8: value = double(*p); break;
        case Type::Int16: value = double(be ? qFromBigEndian<qint16>(p) : qFromLittleEndian<qint16>(p)); break;
        case Type::UInt16: value = double(be ? qFromBigEndian<quint16>(p) : qFromLittleEndian<quint16>(p)); break;
        case Type::Int32: value = double(be ? qFromBigEndian<qint32>(p) : qFromLittleEndian<qint32>(p)); break;
        case Type::UInt32: value = double(be ? qFromBigEndian<quint32>(p) : qFromLittleEndian<quint32>(p)); break;
        case Type::Float32: {
            const quint32 bits = be ? qFromBigEndian<quint32>(p) : qFromLittleEndian<quint32>(p);
            float f;
            std::memcpy(&f, &bits, sizeof f);
            value = double(f);
            break;
        }
        case Type::Float64: {
            const quint64 bits = be ? qFromBigEndian<quint64>(p) : qFromLittleEndian<quint64>(p);
            double d;
            std::memcpy(&d, &bits, sizeof d);
            value = d;
            break;
        }
        case Type::Invalid: return false;
        }
        m_p += size;
        return true;
    }

    bool skip(Type type)
    {
        if (m_format == Format::Ascii) {
            double ignored;
            return readAscii(ignored);
        }
        const int size = typeSize(type);
        if (m_end - m_p < size)
            return false;
        m_p += size;
        return true;
    }

    bool skipProperty(const Property &prop)
    {
        if (!prop.isList)
            return skip(prop.type);
        double count = 0;
        if (!read(prop.countType, count) || count < 0)
            return false;
        for (qint64 i = 0; i < qint64(count); ++i)
            if (!skip(prop.type))
                return false;
        return true;
    }

private:
    bool readAscii(double &value)
    {
        while (m_p < m_end && (*m_p == ' ' || *m_p == '\t' || *m_p == '\n' || *m_p == '\r'))
            ++m_p;
        const char *start = m_p;
        while (m_p < m_end && !(*m_p == ' ' || *m_p == '\t' || *m_p == '\n' || *m_p == '\r'))
            ++m_p;
        if (m_p == start)
            return false;
        bool ok = false;
        // QByteArray::toDouble is locale independent (unlike strtod).
        value = QByteArray::fromRawData(start, int(m_p - start)).toDouble(&ok);
        return ok;
    }

    const char *m_p;
    const char *m_end;
    Format m_format;
};

bool fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return false;
}

} // namespace

bool PlyLoader::load(const QString &path, PointCloud &out, QString *error,
                     const PlyLoadOptions &options)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(error, QStringLiteral("Cannot open %1: %2").arg(path, file.errorString()));
    const qint64 size = file.size();
    if (size <= 0)
        return fail(error, QStringLiteral("%1 is empty").arg(path));
    // Memory-map large files instead of copying them (splat exports can be hundreds of MB).
    if (uchar *mapped = file.map(0, size)) {
        const bool ok = loadFromMemory(reinterpret_cast<const char *>(mapped), size, out, error, options);
        file.unmap(mapped);
        return ok;
    }
    const QByteArray data = file.readAll();
    return loadFromMemory(data.constData(), data.size(), out, error, options);
}

bool PlyLoader::loadFromMemory(const char *data, qint64 size, PointCloud &out, QString *error,
                               const PlyLoadOptions &options)
{
    out = PointCloud();
    const char *end = data + size;

    // ---- header
    const QByteArray headProbe = QByteArray::fromRawData(data, int(qMin<qint64>(size, 1 << 20)));
    if (!headProbe.startsWith("ply"))
        return fail(error, QStringLiteral("Not a PLY file"));
    int headerEnd = headProbe.indexOf("end_header");
    if (headerEnd < 0)
        return fail(error, QStringLiteral("PLY header has no end_header"));
    int bodyStart = headProbe.indexOf('\n', headerEnd);
    if (bodyStart < 0)
        return fail(error, QStringLiteral("Truncated PLY header"));
    ++bodyStart;

    Format format = Format::Ascii;
    bool haveFormat = false;
    std::vector<Element> elements;
    const QList<QByteArray> lines = headProbe.left(headerEnd).split('\n');
    for (const QByteArray &rawLine : lines) {
        const QList<QByteArray> tok = rawLine.simplified().split(' ');
        if (tok.isEmpty() || tok[0].isEmpty())
            continue;
        if (tok[0] == "format" && tok.size() >= 2) {
            haveFormat = true;
            if (tok[1] == "ascii") format = Format::Ascii;
            else if (tok[1] == "binary_little_endian") format = Format::BinaryLittleEndian;
            else if (tok[1] == "binary_big_endian") format = Format::BinaryBigEndian;
            else return fail(error, QStringLiteral("Unknown PLY format: %1").arg(QString::fromLatin1(tok[1])));
        } else if (tok[0] == "element" && tok.size() >= 3) {
            Element e;
            e.name = tok[1];
            e.count = tok[2].toLongLong();
            elements.push_back(e);
        } else if (tok[0] == "property" && !elements.empty()) {
            Property p;
            if (tok.size() >= 5 && tok[1] == "list") {
                p.isList = true;
                p.countType = parseType(tok[2]);
                p.type = parseType(tok[3]);
                p.name = tok[4];
            } else if (tok.size() >= 3) {
                p.type = parseType(tok[1]);
                p.name = tok[2];
            }
            if (p.type == Type::Invalid || (p.isList && p.countType == Type::Invalid))
                return fail(error, QStringLiteral("Unsupported PLY property: %1").arg(QString::fromLatin1(rawLine.trimmed())));
            elements.back().props.push_back(p);
        }
    }
    if (!haveFormat)
        return fail(error, QStringLiteral("PLY header has no format line"));

    // ---- body
    BodyReader reader(data + bodyStart, end, format);
    for (const Element &element : elements) {
        if (element.name != "vertex") {
            for (qint64 i = 0; i < element.count; ++i)
                for (const Property &prop : element.props)
                    if (!reader.skipProperty(prop))
                        return fail(error, QStringLiteral("Truncated PLY data"));
            continue;
        }

        // Indices of the properties we use (-1 if absent).
        auto find = [&element](const char *name) -> int {
            for (std::size_t i = 0; i < element.props.size(); ++i)
                if (element.props[i].name == name && !element.props[i].isList)
                    return int(i);
            return -1;
        };
        const int ix = find("x"), iy = find("y"), iz = find("z");
        if (ix < 0 || iy < 0 || iz < 0)
            return fail(error, QStringLiteral("PLY vertices have no x/y/z"));
        int ir = find("red"), ig = find("green"), ib = find("blue");
        if (ir < 0 || ig < 0 || ib < 0) {
            ir = find("diffuse_red"); ig = find("diffuse_green"); ib = find("diffuse_blue");
        }
        const int idc0 = find("f_dc_0"), idc1 = find("f_dc_1"), idc2 = find("f_dc_2");
        const int iop = find("opacity");
        const bool hasRgb = ir >= 0 && ig >= 0 && ib >= 0;
        const bool hasSh = idc0 >= 0 && idc1 >= 0 && idc2 >= 0;
        out.isGaussianSplat = hasSh;

        auto colorScale = [&element](int index) {
            switch (element.props[std::size_t(index)].type) {
            case Type::UInt8: return 1.0 / 255.0;
            case Type::UInt16: return 1.0 / 65535.0;
            default: return 1.0;
            }
        };
        const double rgbScale = hasRgb ? colorScale(ir) : 1.0;

        std::vector<int> wanted(element.props.size(), 0);
        for (int i : {ix, iy, iz, ir, ig, ib, idc0, idc1, idc2, iop})
            if (i >= 0)
                wanted[std::size_t(i)] = 1;

        out.positions.reserve(std::size_t(element.count) * 3);
        out.colors.reserve(std::size_t(element.count) * 3);
        std::vector<double> values(element.props.size(), 0.0);

        for (qint64 v = 0; v < element.count; ++v) {
            for (std::size_t i = 0; i < element.props.size(); ++i) {
                const Property &prop = element.props[i];
                const bool ok = wanted[i] ? reader.read(prop.type, values[i]) : reader.skipProperty(prop);
                if (!ok)
                    return fail(error, QStringLiteral("Truncated PLY data at vertex %1").arg(v));
            }
            const float x = float(values[std::size_t(ix)]);
            const float y = float(values[std::size_t(iy)]);
            const float z = float(values[std::size_t(iz)]);
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
                continue;
            if (hasSh && iop >= 0) {
                const double alpha = 1.0 / (1.0 + std::exp(-values[std::size_t(iop)]));
                if (alpha < options.minOpacity)
                    continue;
            }
            float r = 0.8f, g = 0.8f, b = 0.8f;
            if (hasSh) {
                r = 0.5f + kShC0 * float(values[std::size_t(idc0)]);
                g = 0.5f + kShC0 * float(values[std::size_t(idc1)]);
                b = 0.5f + kShC0 * float(values[std::size_t(idc2)]);
            } else if (hasRgb) {
                r = float(values[std::size_t(ir)] * rgbScale);
                g = float(values[std::size_t(ig)] * rgbScale);
                b = float(values[std::size_t(ib)] * rgbScale);
            }
            out.positions.insert(out.positions.end(), {x, y, z});
            out.colors.insert(out.colors.end(),
                              {qBound(0.0f, r, 1.0f), qBound(0.0f, g, 1.0f), qBound(0.0f, b, 1.0f)});
        }
        return true; // nothing after the vertices is needed
    }
    return fail(error, QStringLiteral("PLY file has no vertex element"));
}

} // namespace u3d
