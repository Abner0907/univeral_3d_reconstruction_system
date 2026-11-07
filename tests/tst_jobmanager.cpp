#include "JobManager.h"
#include "PlyLoader.h"

#include <QSignalSpy>
#include <QtTest>

using namespace u3d;

class TestJobManager : public QObject
{
    Q_OBJECT

private:
    Settings dryRunSettings(const QString &dataDir) const
    {
        Settings s;
        s.dataDir = dataDir;
        s.dryRun = true;
        s.fakeNsPath = QStringLiteral(U3D_FAKE_NS_PATH);
        return s;
    }

    QStringList makeInputs(const QTemporaryDir &dir, const QStringList &names) const
    {
        QStringList paths;
        for (const QString &name : names) {
            QFile f(dir.filePath(name));
            f.open(QIODevice::WriteOnly);
            f.write(QByteArray(1024, 'x'));
            paths << f.fileName();
        }
        return paths;
    }

private slots:
    void init()
    {
        qputenv("U3D_FAKE_STEP_DELAY", "0.01");
        qunsetenv("U3D_FAKE_FAIL_STAGE");
    }

    void splatfactoSucceeds()
    {
        QTemporaryDir data, inputs;
        JobManager jobs(dryRunSettings(data.path()));
        QSignalSpy logSpy(&jobs, &JobManager::logAppended);
        QString err;
        const QString id = jobs.submit("Mug", "splatfacto", 3000,
                                       makeInputs(inputs, {"a.jpg", "b.jpg", "c.jpg"}), &err);
        QVERIFY2(!id.isEmpty(), qPrintable(err));
        QCOMPARE(jobs.job(id).status, JobStatus::Queued);
        QCOMPARE(jobs.job(id).inputType, QStringLiteral("images"));

        QTRY_COMPARE_WITH_TIMEOUT(jobs.job(id).status, JobStatus::Succeeded, 30000);
        QCOMPARE(jobs.job(id).progress, 1.0);
        QVERIFY(logSpy.count() > 10);

        const QString result = jobs.resultPath(id);
        QVERIFY(result.endsWith("splat.ply"));
        PointCloud pc;
        QVERIFY(PlyLoader::load(result, pc));
        QVERIFY(pc.isGaussianSplat);
        QCOMPARE(pc.size(), std::size_t(4000));

        const QString log = jobs.logTail(id);
        QVERIFY(log.contains("ns-process-data"));
        QVERIFY(log.contains("--max-num-iterations 3000"));
        QVERIFY(log.contains("gaussian-splat"));
    }

    void nerfactoVideoSucceeds()
    {
        QTemporaryDir data, inputs;
        JobManager jobs(dryRunSettings(data.path()));
        QString err;
        const QString id = jobs.submit("Walk", "nerfacto", 1000, makeInputs(inputs, {"walk.mp4"}), &err);
        QVERIFY2(!id.isEmpty(), qPrintable(err));
        QCOMPARE(jobs.job(id).inputType, QStringLiteral("video"));
        QTRY_COMPARE_WITH_TIMEOUT(jobs.job(id).status, JobStatus::Succeeded, 30000);
        QVERIFY(jobs.resultPath(id).endsWith("point_cloud.ply"));
        QVERIFY(jobs.logTail(id).contains("ns-process-data video"));
    }

    void progressIsReported()
    {
        QTemporaryDir data, inputs;
        JobManager jobs(dryRunSettings(data.path()));
        QList<double> progress;
        connect(&jobs, &JobManager::jobChanged, this,
                [&](const QString &id) { progress << jobs.job(id).progress; });
        const QString id = jobs.submit({}, {}, 0, makeInputs(inputs, {"1.png", "2.png", "3.png"}));
        QTRY_COMPARE_WITH_TIMEOUT(jobs.job(id).status, JobStatus::Succeeded, 30000);
        QVERIFY(std::is_sorted(progress.begin(), progress.end()));
        QVERIFY(std::any_of(progress.begin(), progress.end(), [](double p) { return p > 0.3 && p < 0.95; }));
        QCOMPARE(jobs.job(id).method, QStringLiteral("splatfacto")); // default
        QCOMPARE(jobs.job(id).iterations, 30000);
    }

    void failureIsReported()
    {
        qputenv("U3D_FAKE_FAIL_STAGE", "train");
        QTemporaryDir data, inputs;
        JobManager jobs(dryRunSettings(data.path()));
        const QString id = jobs.submit("Fail", "splatfacto", 1000, makeInputs(inputs, {"a.jpg", "b.jpg", "c.jpg"}));
        QTRY_COMPARE_WITH_TIMEOUT(jobs.job(id).status, JobStatus::Failed, 30000);
        QVERIFY(jobs.job(id).message.contains("train"));
        QVERIFY(jobs.resultPath(id).isEmpty());
        QVERIFY(jobs.logTail(id).contains("simulated failure"));
    }

    void missingNerfstudioIsReported()
    {
        QTemporaryDir data, inputs;
        Settings s = dryRunSettings(data.path());
        s.dryRun = false;
        s.nsBinDir = data.filePath("does-not-exist");
        JobManager jobs(s);
        const QString id = jobs.submit("NoNs", "splatfacto", 1000, makeInputs(inputs, {"a.jpg", "b.jpg", "c.jpg"}));
        QTRY_COMPARE_WITH_TIMEOUT(jobs.job(id).status, JobStatus::Failed, 30000);
        QVERIFY(jobs.job(id).message.contains("Could not start"));
    }

    void cancelRunningJob()
    {
        qputenv("U3D_FAKE_STEP_DELAY", "0.3");
        QTemporaryDir data, inputs;
        JobManager jobs(dryRunSettings(data.path()));
        const QStringList files = makeInputs(inputs, {"a.jpg", "b.jpg", "c.jpg"});
        const QString first = jobs.submit("First", "splatfacto", 1000, files);
        const QString second = jobs.submit("Second", "splatfacto", 1000, files);
        QTRY_COMPARE_WITH_TIMEOUT(jobs.job(first).stage, QStringLiteral("process_data"), 10000);
        QCOMPARE(jobs.job(second).status, JobStatus::Queued); // one job at a time

        QVERIFY(jobs.cancelJob(second));
        QCOMPARE(jobs.job(second).status, JobStatus::Cancelled);
        QVERIFY(jobs.cancelJob(first));
        QTRY_COMPARE_WITH_TIMEOUT(jobs.job(first).status, JobStatus::Cancelled, 15000);
    }

    void removeDeletesFiles()
    {
        QTemporaryDir data, inputs;
        JobManager jobs(dryRunSettings(data.path()));
        QSignalSpy removed(&jobs, &JobManager::jobRemoved);
        const QString id = jobs.submit("Gone", "splatfacto", 1000, makeInputs(inputs, {"a.jpg", "b.jpg", "c.jpg"}));
        QTRY_COMPARE_WITH_TIMEOUT(jobs.job(id).status, JobStatus::Succeeded, 30000);
        const QString dir = jobs.jobDir(id);
        QVERIFY(QFileInfo::exists(dir));
        QVERIFY(jobs.removeJob(id));
        QCOMPARE(removed.count(), 1);
        QVERIFY(!jobs.hasJob(id));
        QVERIFY(!QFileInfo::exists(dir));
    }

    void jobsSurviveRestart()
    {
        QTemporaryDir data, inputs;
        QString done, pending;
        {
            JobManager jobs(dryRunSettings(data.path()));
            done = jobs.submit("Done", "splatfacto", 1000, makeInputs(inputs, {"a.jpg", "b.jpg", "c.jpg"}));
            QTRY_COMPARE_WITH_TIMEOUT(jobs.job(done).status, JobStatus::Succeeded, 30000);
            pending = jobs.createJob("Half uploaded", "nerfacto", 1000);
            QVERIFY(!jobs.addInput(pending, "x.jpg", "data").isEmpty());
        }
        JobManager reloaded(dryRunSettings(data.path()));
        QCOMPARE(reloaded.jobs().size(), 2);
        QCOMPARE(reloaded.job(done).status, JobStatus::Succeeded);
        QVERIFY(!reloaded.resultPath(done).isEmpty());
        QCOMPARE(reloaded.job(pending).status, JobStatus::Failed); // upload never finished
    }

    void inputValidation()
    {
        QTemporaryDir data;
        JobManager jobs(dryRunSettings(data.path()));
        QString err;
        QVERIFY(jobs.createJob("x", "instant-ngp", 1000, &err).isEmpty());
        QVERIFY(err.contains("Unknown method"));
        QVERIFY(jobs.createJob("x", "splatfacto", 5, &err).isEmpty());

        const QString id = jobs.createJob("x", "splatfacto", 1000, &err);
        QVERIFY(!id.isEmpty());
        QVERIFY(jobs.addInput(id, "evil.exe", "MZ", &err).isEmpty());
        QCOMPARE(jobs.addInput(id, "../../IMG.jpg", "1"), QStringLiteral("IMG.jpg"));
        QCOMPARE(jobs.addInput(id, "IMG.jpg", "2"), QStringLiteral("IMG_1.jpg")); // no overwrite
        QVERIFY(!jobs.startJob(id, &err)); // only 2 images
        QVERIFY(err.contains("3 images"));
        QVERIFY(!jobs.addInput(id, "IMG.jpg", "3").isEmpty());
        QVERIFY(jobs.startJob(id, &err));
        QVERIFY(jobs.addInput(id, "late.jpg", "4", &err).isEmpty()); // already started
    }
};

QTEST_GUILESS_MAIN(TestJobManager)
#include "tst_jobmanager.moc"
