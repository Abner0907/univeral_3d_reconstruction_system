#include "PlyLoader.h"

#include <QtEndian>
#include <QtTest>

#include <cstring>

using namespace u3d;

namespace {

void appendFloat(QByteArray &out, float v, bool bigEndian = false)
{
    quint32 bits;
    std::memcpy(&bits, &v, 4);
    char buf[4];
    if (bigEndian)
        qToBigEndian(bits, buf);
    else
        qToLittleEndian(bits, buf);
    out.append(buf, 4);
}

} // namespace

class TestPlyLoader : public QObject
{
    Q_OBJECT

private slots:
    void asciiPointCloud()
    {
        const QByteArray ply =
            "ply\nformat ascii 1.0\ncomment test\nelement vertex 2\n"
            "property float x\nproperty float y\nproperty float z\n"
            "property uchar red\nproperty uchar green\nproperty uchar blue\n"
            "element face 1\nproperty list uchar int vertex_indices\nend_header\n"
            "0 1 2 255 0 0\n-1.5 0.25 3e1 0 255 51\n3 0 1 1\n";
        PointCloud pc;
        QString err;
        QVERIFY2(PlyLoader::loadFromMemory(ply.constData(), ply.size(), pc, &err), qPrintable(err));
        QCOMPARE(pc.size(), std::size_t(2));
        QVERIFY(!pc.isGaussianSplat);
        QCOMPARE(pc.positions[3], -1.5f);
        QCOMPARE(pc.positions[5], 30.0f);
        QCOMPARE(pc.colors[0], 1.0f);
        QVERIFY(qAbs(pc.colors[5] - 0.2f) < 1e-6f);
    }

    void binaryGaussianSplat()
    {
        // x y z f_dc_0..2 opacity; second splat is nearly transparent and must be skipped.
        QByteArray ply = "ply\nformat binary_little_endian 1.0\nelement vertex 2\n"
                         "property float x\nproperty float y\nproperty float z\n"
                         "property float f_dc_0\nproperty float f_dc_1\nproperty float f_dc_2\n"
                         "property float opacity\nproperty float scale_0\nend_header\n";
        for (float v : {1.f, 2.f, 3.f, 0.f, 1.f, -1.f, 5.f, 0.f})
            appendFloat(ply, v);
        for (float v : {9.f, 9.f, 9.f, 0.f, 0.f, 0.f, -10.f, 0.f})
            appendFloat(ply, v);

        PointCloud pc;
        QString err;
        QVERIFY2(PlyLoader::loadFromMemory(ply.constData(), ply.size(), pc, &err), qPrintable(err));
        QVERIFY(pc.isGaussianSplat);
        QCOMPARE(pc.size(), std::size_t(1));
        QCOMPARE(pc.positions[2], 3.0f);
        const float c0 = 0.28209479177387814f;
        QVERIFY(qAbs(pc.colors[0] - 0.5f) < 1e-6f);
        QVERIFY(qAbs(pc.colors[1] - (0.5f + c0)) < 1e-6f);
        QVERIFY(qAbs(pc.colors[2] - (0.5f - c0)) < 1e-6f);
    }

    void binaryBigEndianWithLeadingElement()
    {
        QByteArray ply = "ply\nformat binary_big_endian 1.0\n"
                         "element camera 1\nproperty list uchar float k\n"
                         "element vertex 1\nproperty double x\nproperty float y\nproperty float z\n"
                         "end_header\n";
        ply.append(char(2));
        appendFloat(ply, 1.f, true);
        appendFloat(ply, 2.f, true);
        const double x = 4.5;
        quint64 bits;
        std::memcpy(&bits, &x, 8);
        char buf[8];
        qToBigEndian(bits, buf);
        ply.append(buf, 8);
        appendFloat(ply, -2.f, true);
        appendFloat(ply, 7.f, true);

        PointCloud pc;
        QString err;
        QVERIFY2(PlyLoader::loadFromMemory(ply.constData(), ply.size(), pc, &err), qPrintable(err));
        QCOMPARE(pc.size(), std::size_t(1));
        QCOMPARE(pc.positions[0], 4.5f);
        QCOMPARE(pc.positions[1], -2.0f);
        QCOMPARE(pc.positions[2], 7.0f);
    }

    void errors()
    {
        PointCloud pc;
        QString err;
        const QByteArray notPly = "hello";
        QVERIFY(!PlyLoader::loadFromMemory(notPly.constData(), notPly.size(), pc, &err));
        const QByteArray truncated =
            "ply\nformat binary_little_endian 1.0\nelement vertex 10\n"
            "property float x\nproperty float y\nproperty float z\nend_header\nabc";
        QVERIFY(!PlyLoader::loadFromMemory(truncated.constData(), truncated.size(), pc, &err));
        QVERIFY(err.contains("Truncated"));
        const QByteArray noXyz = "ply\nformat ascii 1.0\nelement vertex 1\nproperty float a\nend_header\n1\n";
        QVERIFY(!PlyLoader::loadFromMemory(noXyz.constData(), noXyz.size(), pc, &err));
    }

    void fromFile()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("cloud.ply");
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("ply\nformat ascii 1.0\nelement vertex 3\nproperty float x\nproperty float y\n"
                "property float z\nend_header\n0 0 0\n1 0 0\n0 1 0\n");
        f.close();
        PointCloud pc;
        QVERIFY(PlyLoader::load(path, pc));
        QCOMPARE(pc.size(), std::size_t(3));
        QCOMPARE(pc.colors[0], 0.8f); // default grey
    }
};

QTEST_APPLESS_MAIN(TestPlyLoader)
#include "tst_plyloader.moc"
