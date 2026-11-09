#include "u3d/splat/SplatPly.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <locale>
#include <sstream>

namespace u3d {

namespace {

bool fail(std::string *error, const std::string &message)
{
    if (error)
        *error = message;
    return false;
}

enum class Type { I8, U8, I16, U16, I32, U32, F32, F64, Invalid };
enum class Format { Ascii, BinaryLE, BinaryBE };

Type parseType(const std::string &t)
{
    if (t == "char" || t == "int8") return Type::I8;
    if (t == "uchar" || t == "uint8") return Type::U8;
    if (t == "short" || t == "int16") return Type::I16;
    if (t == "ushort" || t == "uint16") return Type::U16;
    if (t == "int" || t == "int32") return Type::I32;
    if (t == "uint" || t == "uint32") return Type::U32;
    if (t == "float" || t == "float32") return Type::F32;
    if (t == "double" || t == "float64") return Type::F64;
    return Type::Invalid;
}

int typeSize(Type t)
{
    switch (t) {
    case Type::I8: case Type::U8: return 1;
    case Type::I16: case Type::U16: return 2;
    case Type::I32: case Type::U32: case Type::F32: return 4;
    case Type::F64: return 8;
    default: return 0;
    }
}

bool hostIsLittleEndian()
{
    const std::uint16_t v = 1;
    std::uint8_t b;
    std::memcpy(&b, &v, 1);
    return b == 1;
}

struct Property {
    std::string name;
    Type type = Type::Invalid;
    bool isList = false;
    Type countType = Type::Invalid;
};

struct Element {
    std::string name;
    std::size_t count = 0;
    std::vector<Property> props;
};

class Reader
{
public:
    Reader(const char *p, const char *end, Format f) : m_p(p), m_end(end), m_format(f) {}

    bool read(Type t, double &out)
    {
        if (m_format == Format::Ascii) {
            while (m_p < m_end && std::isspace(static_cast<unsigned char>(*m_p)))
                ++m_p;
            const char *start = m_p;
            while (m_p < m_end && !std::isspace(static_cast<unsigned char>(*m_p)))
                ++m_p;
            if (m_p == start)
                return false;
            std::istringstream ss(std::string(start, m_p));
            ss.imbue(std::locale::classic());
            ss >> out;
            return !ss.fail();
        }
        const int size = typeSize(t);
        if (size == 0 || m_end - m_p < size)
            return false;
        unsigned char buf[8];
        std::memcpy(buf, m_p, std::size_t(size));
        m_p += size;
        if ((m_format == Format::BinaryLE) != hostIsLittleEndian())
            std::reverse(buf, buf + size);
        switch (t) {
        case Type::I8: { std::int8_t v; std::memcpy(&v, buf, 1); out = v; break; }
        case Type::U8: out = buf[0]; break;
        case Type::I16: { std::int16_t v; std::memcpy(&v, buf, 2); out = v; break; }
        case Type::U16: { std::uint16_t v; std::memcpy(&v, buf, 2); out = v; break; }
        case Type::I32: { std::int32_t v; std::memcpy(&v, buf, 4); out = v; break; }
        case Type::U32: { std::uint32_t v; std::memcpy(&v, buf, 4); out = v; break; }
        case Type::F32: { float v; std::memcpy(&v, buf, 4); out = v; break; }
        case Type::F64: { double v; std::memcpy(&v, buf, 8); out = v; break; }
        default: return false;
        }
        return true;
    }

    bool skip(const Property &p)
    {
        double v;
        if (!p.isList)
            return m_format == Format::Ascii ? read(p.type, v) : advance(typeSize(p.type));
        double count;
        if (!read(p.countType, count) || count < 0)
            return false;
        for (std::size_t k = 0; k < std::size_t(count); ++k)
            if (!(m_format == Format::Ascii ? read(p.type, v) : advance(typeSize(p.type))))
                return false;
        return true;
    }

private:
    bool advance(int n)
    {
        if (m_end - m_p < n)
            return false;
        m_p += n;
        return true;
    }
    const char *m_p;
    const char *m_end;
    Format m_format;
};

// Reads the vertex element; `columns` receives one row of `wanted` values per vertex (NaN when a
// property is missing), `found[k]` tells whether wanted[k] exists.
bool readVertexTable(const std::string &path, const std::vector<std::string> &wanted,
                     std::vector<float> &columns, std::vector<bool> &found, std::size_t &count,
                     std::string *error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return fail(error, "Cannot open " + path);
    std::vector<char> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::string headProbe(data.data(), std::min<std::size_t>(data.size(), 1 << 16));
    if (headProbe.compare(0, 3, "ply") != 0)
        return fail(error, "Not a PLY file: " + path);
    const std::size_t endHeader = headProbe.find("end_header");
    if (endHeader == std::string::npos)
        return fail(error, "PLY header has no end_header");
    const std::size_t bodyStart = headProbe.find('\n', endHeader);
    if (bodyStart == std::string::npos)
        return fail(error, "Truncated PLY header");

    Format format = Format::Ascii;
    std::vector<Element> elements;
    std::istringstream header(headProbe.substr(0, endHeader));
    std::string line;
    while (std::getline(header, line)) {
        std::istringstream ls(line);
        std::string word;
        ls >> word;
        if (word == "format") {
            std::string f;
            ls >> f;
            if (f == "ascii") format = Format::Ascii;
            else if (f == "binary_little_endian") format = Format::BinaryLE;
            else if (f == "binary_big_endian") format = Format::BinaryBE;
            else return fail(error, "Unknown PLY format " + f);
        } else if (word == "element") {
            Element e;
            ls >> e.name >> e.count;
            elements.push_back(e);
        } else if (word == "property" && !elements.empty()) {
            Property p;
            std::string a, b, c;
            ls >> a >> b >> c;
            if (a == "list") {
                p.isList = true;
                p.countType = parseType(b);
                p.type = parseType(c);
                ls >> p.name;
            } else {
                p.type = parseType(a);
                p.name = b;
            }
            if (p.type == Type::Invalid)
                return fail(error, "Unsupported PLY property: " + line);
            elements.back().props.push_back(p);
        }
    }

    Reader reader(data.data() + bodyStart + 1, data.data() + data.size(), format);
    for (const Element &e : elements) {
        if (e.name != "vertex") {
            for (std::size_t i = 0; i < e.count; ++i)
                for (const Property &p : e.props)
                    if (!reader.skip(p))
                        return fail(error, "Truncated PLY data");
            continue;
        }
        std::vector<int> column(e.props.size(), -1);
        found.assign(wanted.size(), false);
        for (std::size_t k = 0; k < wanted.size(); ++k)
            for (std::size_t p = 0; p < e.props.size(); ++p)
                if (!e.props[p].isList && e.props[p].name == wanted[k]) {
                    column[p] = int(k);
                    found[k] = true;
                }
        count = e.count;
        columns.assign(e.count * wanted.size(), std::nanf(""));
        for (std::size_t i = 0; i < e.count; ++i)
            for (std::size_t p = 0; p < e.props.size(); ++p) {
                if (column[p] < 0) {
                    if (!reader.skip(e.props[p]))
                        return fail(error, "Truncated PLY data at vertex " + std::to_string(i));
                    continue;
                }
                double v;
                if (!reader.read(e.props[p].type, v))
                    return fail(error, "Truncated PLY data at vertex " + std::to_string(i));
                const std::string &name = wanted[std::size_t(column[p])];
                if (name == "red" || name == "green" || name == "blue") { // normalise colours to 0..1
                    if (e.props[p].type == Type::U8) v /= 255.0;
                    else if (e.props[p].type == Type::U16) v /= 65535.0;
                }
                columns[i * wanted.size() + std::size_t(column[p])] = float(v);
            }
        return true;
    }
    return fail(error, "PLY file has no vertex element");
}

void writeHeader(std::ofstream &out, std::size_t count, const std::vector<std::string> &props)
{
    out << "ply\nformat binary_little_endian 1.0\n";
    out << "comment Universal 3D Reconstruction System\n";
    out << "element vertex " << count << "\n";
    for (const std::string &p : props)
        out << "property " << p << "\n";
    out << "end_header\n";
}

void writeFloatLE(std::ofstream &out, float v)
{
    unsigned char b[4];
    std::memcpy(b, &v, 4);
    if (!hostIsLittleEndian())
        std::reverse(b, b + 4);
    out.write(reinterpret_cast<const char *>(b), 4);
}

} // namespace

bool writeSplatPly(const std::string &path, const GaussianModel &m, std::string *error)
{
    std::ofstream out(path, std::ios::binary);
    if (!out)
        return fail(error, "Cannot write " + path);
    writeHeader(out, m.size(),
                {"float x", "float y", "float z", "float nx", "float ny", "float nz",
                 "float f_dc_0", "float f_dc_1", "float f_dc_2", "float opacity",
                 "float scale_0", "float scale_1", "float scale_2",
                 "float rot_0", "float rot_1", "float rot_2", "float rot_3"});
    for (std::size_t i = 0; i < m.size(); ++i) {
        for (int k = 0; k < 3; ++k) writeFloatLE(out, m.means[i * 3 + k]);
        for (int k = 0; k < 3; ++k) writeFloatLE(out, 0.0f);
        for (int k = 0; k < 3; ++k) writeFloatLE(out, m.colors[i * 3 + k]);
        writeFloatLE(out, m.opacities[i]);
        for (int k = 0; k < 3; ++k) writeFloatLE(out, m.scales[i * 3 + k]);
        for (int k = 0; k < 4; ++k) writeFloatLE(out, m.quats[i * 4 + k]);
    }
    if (!out)
        return fail(error, "Write error: " + path);
    return true;
}

bool readSplatPly(const std::string &path, GaussianModel &model, std::string *error, bool *isSplat)
{
    const std::vector<std::string> wanted = {"x", "y", "z", "f_dc_0", "f_dc_1", "f_dc_2", "opacity",
                                             "scale_0", "scale_1", "scale_2", "rot_0", "rot_1", "rot_2",
                                             "rot_3", "red", "green", "blue"};
    std::vector<float> table;
    std::vector<bool> found;
    std::size_t count = 0;
    if (!readVertexTable(path, wanted, table, found, count, error))
        return false;
    if (!found[0] || !found[1] || !found[2])
        return fail(error, "PLY vertices have no x/y/z");
    const bool splat = found[3] && found[6] && found[7] && found[10];
    if (isSplat)
        *isSplat = splat;
    const std::size_t W = wanted.size();

    if (!splat) {
        std::vector<float> xyz, rgb;
        xyz.reserve(count * 3);
        rgb.reserve(count * 3);
        for (std::size_t i = 0; i < count; ++i) {
            const float *r = &table[i * W];
            if (!std::isfinite(r[0]) || !std::isfinite(r[1]) || !std::isfinite(r[2]))
                continue;
            xyz.insert(xyz.end(), {r[0], r[1], r[2]});
            if (found[14])
                rgb.insert(rgb.end(), {r[14], r[15], r[16]});
            else
                rgb.insert(rgb.end(), {0.8f, 0.8f, 0.8f});
        }
        model = initGaussiansFromPoints(xyz, rgb);
        for (float &o : model.opacities)
            o = logit(0.95f);
        for (float &s : model.scales)
            s += std::log(0.5f);
        return true;
    }

    model = GaussianModel();
    model.resize(count);
    std::size_t out = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const float *r = &table[i * W];
        if (!std::isfinite(r[0]) || !std::isfinite(r[1]) || !std::isfinite(r[2]))
            continue;
        for (int k = 0; k < 3; ++k) {
            model.means[out * 3 + k] = r[k];
            model.colors[out * 3 + k] = r[3 + k];
            model.scales[out * 3 + k] = r[7 + k];
        }
        model.opacities[out] = r[6];
        for (int k = 0; k < 4; ++k)
            model.quats[out * 4 + k] = r[10 + k];
        ++out;
    }
    model.resize(out);
    return true;
}

bool writePointsPly(const std::string &path, const std::vector<float> &xyz, const std::vector<float> &rgb,
                    std::string *error)
{
    std::ofstream out(path, std::ios::binary);
    if (!out)
        return fail(error, "Cannot write " + path);
    const std::size_t n = xyz.size() / 3;
    writeHeader(out, n, {"float x", "float y", "float z", "uchar red", "uchar green", "uchar blue"});
    for (std::size_t i = 0; i < n; ++i) {
        for (int k = 0; k < 3; ++k)
            writeFloatLE(out, xyz[i * 3 + k]);
        for (int k = 0; k < 3; ++k) {
            const float c = rgb.size() >= (i + 1) * 3 ? rgb[i * 3 + k] : 0.8f;
            const unsigned char b = static_cast<unsigned char>(std::lround(std::min(1.0f, std::max(0.0f, c)) * 255.0f));
            out.write(reinterpret_cast<const char *>(&b), 1);
        }
    }
    return bool(out) || fail(error, "Write error: " + path);
}

bool readPointsPly(const std::string &path, std::vector<float> &xyz, std::vector<float> &rgb, std::string *error)
{
    std::vector<float> table;
    std::vector<bool> found;
    std::size_t count = 0;
    if (!readVertexTable(path, {"x", "y", "z", "red", "green", "blue"}, table, found, count, error))
        return false;
    if (!found[0] || !found[1] || !found[2])
        return fail(error, "PLY vertices have no x/y/z");
    xyz.clear();
    rgb.clear();
    for (std::size_t i = 0; i < count; ++i) {
        const float *r = &table[i * 6];
        if (!std::isfinite(r[0]) || !std::isfinite(r[1]) || !std::isfinite(r[2]))
            continue;
        xyz.insert(xyz.end(), {r[0], r[1], r[2]});
        if (found[3])
            rgb.insert(rgb.end(), {r[3], r[4], r[5]});
        else
            rgb.insert(rgb.end(), {0.8f, 0.8f, 0.8f});
    }
    return true;
}

} // namespace u3d
