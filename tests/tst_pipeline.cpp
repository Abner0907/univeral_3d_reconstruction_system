#include "Pipeline.h"

#include <QtTest>

using namespace u3d;
using namespace u3d::pipeline;

class TestPipeline : public QObject
{
    Q_OBJECT

private slots:
    void trainProgress_data()
    {
        QTest::addColumn<QString>("line");
        QTest::addColumn<double>("expected");
        QTest::newRow("table row") << "12340 (41.13%)       45.123 ms   1 h, 2 m" << 0.4113;
        QTest::newRow("leading space") << "   29999 (100.00%)   1 ms" << 1.0;
        QTest::newRow("ansi") << "\x1b[1m300 (1.00%)\x1b[0m  10 ms" << 0.01;
        QTest::newRow("header") << "Step (% Done)  Train Iter (time)" << -1.0;
        QTest::newRow("other") << "Saving checkpoint to step-000001000.ckpt" << -1.0;
    }

    void trainProgress()
    {
        QFETCH(QString, line);
        QFETCH(double, expected);
        QVERIFY(qAbs(parseTrainProgress(line) - expected) < 1e-6);
    }

    void classify()
    {
        QString err;
        QCOMPARE(classifyInputs({"a.jpg", "b.JPG", "c.png"}, &err), QStringLiteral("images"));
        QCOMPARE(classifyInputs({"walk.mp4"}, &err), QStringLiteral("video"));
        QVERIFY(classifyInputs({"a.jpg", "b.jpg"}, &err).isEmpty());
        QVERIFY(err.contains("3 images"));
        QVERIFY(classifyInputs({"a.jpg", "b.jpg", "c.jpg", "d.mp4"}, &err).isEmpty());
        QVERIFY(classifyInputs({"a.heic", "b.jpg", "c.jpg"}, &err).isEmpty());
        QVERIFY(err.contains("a.heic"));
        QVERIFY(classifyInputs({}, &err).isEmpty());
        QVERIFY(classifyInputs({"a.mp4", "b.mov"}, &err).isEmpty());
    }

    void sanitize()
    {
        QCOMPARE(sanitizeFileName("../../etc/passwd", "x"), QStringLiteral("passwd"));
        QCOMPARE(sanitizeFileName("C:\\Users\\me\\IMG 01.jpg", "x"), QStringLiteral("IMG_01.jpg"));
        QCOMPARE(sanitizeFileName("..", "fallback"), QStringLiteral("fallback"));
        QCOMPARE(sanitizeFileName(".hidden.png", "x"), QStringLiteral("hidden.png"));
    }

    void commands()
    {
        Settings s;
        s.nsBinDir = QStringLiteral("/opt/ns/bin");
        s.vis = QStringLiteral("viewer");
        s.viewerPort = 7010;
        const JobPaths p{QStringLiteral("/data/jobs/abc")};

        const Command process = processDataCommand(s, p, QStringLiteral("images"));
        QCOMPARE(process.program, QStringLiteral("/opt/ns/bin/ns-process-data"));
        QCOMPARE(process.arguments.mid(0, 5),
                 QStringList({"images", "--data", "/data/jobs/abc/input", "--output-dir",
                              "/data/jobs/abc/processed"}));

        const Command train = trainCommand(s, p, QStringLiteral("abc"), QStringLiteral("splatfacto"), 7000);
        QCOMPARE(train.program, QStringLiteral("/opt/ns/bin/ns-train"));
        QCOMPARE(train.arguments.first(), QStringLiteral("splatfacto"));
        QVERIFY(train.arguments.join(' ').contains("--max-num-iterations 7000"));
        QVERIFY(train.arguments.join(' ').contains("--viewer.quit-on-train-completion True"));
        QVERIFY(train.arguments.join(' ').contains("--viewer.websocket-port 7010"));

        s.vis = QStringLiteral("tensorboard");
        const Command headless = trainCommand(s, p, QStringLiteral("abc"), QStringLiteral("nerfacto"), 7000);
        QVERIFY(!headless.arguments.join(' ').contains("viewer."));

        const Command splat = exportCommand(s, p, QStringLiteral("splatfacto"), QStringLiteral("/c.yml"));
        QCOMPARE(splat.arguments.first(), QStringLiteral("gaussian-splat"));
        const Command cloud = exportCommand(s, p, QStringLiteral("nerfacto"), QStringLiteral("/c.yml"));
        QCOMPARE(cloud.arguments.first(), QStringLiteral("pointcloud"));
        QVERIFY(cloud.arguments.contains(QStringLiteral("--normal-method")));

        s.dryRun = true;
        s.fakeNsPath = QStringLiteral("/bin/fake");
        const Command fake = toolCommand(s, QStringLiteral("ns-train"));
        QCOMPARE(fake.program, QStringLiteral("/bin/fake"));
        QCOMPARE(fake.arguments, QStringList{"ns-train"});
    }

    void methods()
    {
        QVERIFY(isValidMethod("splatfacto"));
        QVERIFY(isSplatMethod("splatfacto-big"));
        QVERIFY(!isSplatMethod("nerfacto"));
        QVERIFY(!isValidMethod("instant-ngp"));
    }
};

QTEST_APPLESS_MAIN(TestPipeline)
#include "tst_pipeline.moc"
