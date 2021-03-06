/*
    SPDX-FileCopyrightText: 2014 Daniel Vrátil <dvratil@redhat.com>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

// Krazy mistakes job.exec() for QDialog::exec() and urges us to use QPointer
//krazy:excludeall=crashy

#include "../src/async.h"

#include <QObject>
#include <QString>
#include <QTimer>
#include <QtTest/QTest>
#include <QDebug>

#include <functional>

#define COMPARERET(actual, expected, retval) \
do {\
    if (!QTest::qCompare(actual, expected, #actual, #expected, __FILE__, __LINE__))\
        return retval;\
} while (0)

#define VERIFYRET(statement, retval) \
do {\
    if (!QTest::qVerify((statement), #statement, "", __FILE__, __LINE__))\
        return retval;\
} while (0)

class AsyncTest : public QObject
{
    Q_OBJECT

public:
    AsyncTest()
    {}

    ~AsyncTest()
    {}

private Q_SLOTS:
    void testStart2();
    void testSyncPromises();
    void testErrorHandling();
    void testContext();
    void testGuard();
    void testDoWhile();
    void testDoWhileWithJob();
    void testNestedDoWhile();
    void testAsyncPromises();
    void testNestedAsync();
    void testVoidNestedJob();
    void testAsyncEach();
    void testAsyncSerialEach();
    void noTemplateArguments();
    void testValueJob();

    void benchmarkSyncThenExecutor();
    void benchmarkFutureThenExecutor();
    void benchmarkThenExecutor();

private:
    template<typename T>
    class AsyncSimulator {
    public:
        AsyncSimulator(KAsync::Future<T> &future, const T &result)
            : mFuture(future)
            , mResult(result)
        {
            QObject::connect(&mTimer, &QTimer::timeout,
                             [this]() {
                                 mFuture.setValue(mResult);
                                 mFuture.setFinished();
                             });
            QObject::connect(&mTimer, &QTimer::timeout,
                             [this]() {
                                 delete this;
                             });
            mTimer.setSingleShot(true);
            mTimer.start(200);
        }

        AsyncSimulator(KAsync::Future<T> &future, std::function<void(KAsync::Future<T>&)> callback)
            : mFuture(future)
            , mCallback(callback)
        {
            QObject::connect(&mTimer, &QTimer::timeout,
                             [this]() {
                                 mCallback(mFuture);
                             });
            QObject::connect(&mTimer, &QTimer::timeout,
                             [this]() {
                                 delete this;
                             });
            mTimer.setSingleShot(true);
            mTimer.start(200);
        }

    private:
        KAsync::Future<T> mFuture;
        std::function<void(KAsync::Future<T>&)> mCallback;
        T mResult;
        QTimer mTimer;
    };

    class MemberTest
    {
    public:
        MemberTest()
            : mFoo(-1)
        {
        }

        void syncFoo(int foo)
        {
            mFoo = foo;
        }

        int syncFooRet(int foo)
        {
            return ++foo;
        }

        void asyncFoo(int foo, KAsync::Future<int> &future)
        {
            new AsyncSimulator<int>(future, ++foo);
        }

        int mFoo;
    };
};

template<>
class AsyncTest::AsyncSimulator<void> {
public:
    AsyncSimulator(KAsync::Future<void> &future)
        : mFuture(future)
    {
        QObject::connect(&mTimer, &QTimer::timeout,
                            [this]() {
                                mFuture.setFinished();
                            });
        QObject::connect(&mTimer, &QTimer::timeout,
                            [this]() {
                                delete this;
                            });
        mTimer.setSingleShot(true);
        mTimer.start(200);
    }

private:
    KAsync::Future<void> mFuture;
    QTimer mTimer;
};

void AsyncTest::testStart2()
{
    {
        auto future = KAsync::start<int>(
            []() {
                return 42;
            }).exec();
        QVERIFY(future.isFinished());
        QCOMPARE(future.value(), 42);
    }
    {
        auto future = KAsync::start<int, int>(
            [](int i) {
                return i;
            }).exec(42);
        QVERIFY(future.isFinished());
        QCOMPARE(future.value(), 42);
    }
    {
        bool called = false;
        auto future = KAsync::start<void>(
            [&]() {
                called = true;
            }).exec();
        QVERIFY(future.isFinished());
        QVERIFY(called);
    }
    {
        auto future = KAsync::start<int>(
            []() {
                return KAsync::value(42);
            }).exec();
        QVERIFY(future.isFinished());
        QCOMPARE(future.value(), 42);
    }
    {
        auto future = KAsync::start<int, int>(
            [](int i) {
                return KAsync::value(i);
            }).exec(42);
        QVERIFY(future.isFinished());
        QCOMPARE(future.value(), 42);
    }
    {
        auto future = KAsync::start<int>(
            [](KAsync::Future<int> &f) {
                f.setResult(42);
            }).exec();
        QVERIFY(future.isFinished());
        QCOMPARE(future.value(), 42);
    }

}


void AsyncTest::testSyncPromises()
{
    {
        auto future = KAsync::start<int>(
            []() {
                return 42;
            }).exec();
        QVERIFY(future.isFinished());
        QCOMPARE(future.value(), 42);
    }

    {
        auto future = KAsync::start<int>(
            [](KAsync::Future<int> &f) {
                f.setResult(42);
            }).exec();
        QVERIFY(future.isFinished());
        QCOMPARE(future.value(), 42);
    }

    //Sync start
    {
        auto future = KAsync::start<int>([] {
                return KAsync::value<int>(42);
            }).exec();
        QVERIFY(future.isFinished());
        QCOMPARE(future.value(), 42);
    }

    //Sync start
    {
        bool called = false;
        auto future = KAsync::start<void>(
            [&called] {
                called = true;
                return KAsync::null<void>();
            }).exec();
        QVERIFY(future.isFinished());
        QVERIFY(called);
    }
    //void
    {
        auto future = KAsync::start<void>(
            []() {
                return KAsync::null<void>();
            }).exec();
        QVERIFY(future.isFinished());
    }

    //value
    {
        auto future = KAsync::value<int>(42).exec();
        QVERIFY(future.isFinished());
        QCOMPARE(future.value(), 42);
    }

    //Sync then
    {
        auto job = KAsync::value<int>(42);
        auto future = job.then<int, int>([](int value) {
            return KAsync::value<int>(value);
        }).exec();
        QVERIFY(future.isFinished());
        QCOMPARE(future.value(), 42);
    }

    //Job then
    {
        auto job = KAsync::value<int>(42);
        auto future = job.then<QString, int>([](int value) {
            return KAsync::value<QString>(QString::number(value));
        }).exec();
        QVERIFY(future.isFinished());
        QCOMPARE(future.value(), QString::number(42));
    }

    //void Job then
    {
        bool continuationCalled = false;
        auto job = KAsync::null<void>();
        auto future = job.then<void>([&continuationCalled] {
            return KAsync::start<void>([&continuationCalled] {
                continuationCalled = true;
                return KAsync::null<void>();
            });
        }).exec();
        QVERIFY(future.isFinished());
        QVERIFY(continuationCalled);
    }

    //Nested job then
    {
        auto job = KAsync::value<int>(42);
        auto future = job.then<QString, int>(
            KAsync::start<QString, int>([](int i) {
                return KAsync::value<QString>(QString::number(i));
            })
        ).exec();
        QVERIFY(future.isFinished());
        QCOMPARE(future.value(), QString::number(42));
    }

    //Convert to void
    {
        KAsync::Job<void> job = KAsync::start<int>(
            [] {
                return KAsync::value<int>(42);
            }).then<int, int>([](int i) {
                return KAsync::value<int>(i);
            });
        KAsync::Future<void> future = job.exec();
        QVERIFY(future.isFinished());
    }

    //Job then types
    {
        KAsync::Job<int, double> job1 = KAsync::start<int, double>(
            [](double i)  {
                return KAsync::value<int>(i);
            });

        KAsync::Job<QString, double> job2 = job1.then<QString, int>([](int value) {
            return KAsync::start<QString>([value]() {
                return KAsync::value<QString>(QString::number(value));
            });
        });
        double input = 42;
        KAsync::Future<QString> future = job2.exec(input);
        QVERIFY(future.isFinished());
        QCOMPARE(future.value(), QString::number(42));
    }

    //This is useful to be able to spawn different subjobs depending on the initial input value that the continuation gets.
    {
        auto future = KAsync::start<int, bool>(
            [] (bool i) {
                if (i) {
                    return KAsync::value(42);
                } else {
                    return KAsync::error<int>(KAsync::Error("foo"));
                }
            }).exec(true);
        QVERIFY(future.isFinished());
        QCOMPARE(future.value(), 42);
    }

    {
        auto baseJob = KAsync::value<int>(42)
        .then<QString, int>(
            [](int v, KAsync::Future<QString> &f) {
                f.setValue(QLatin1String("Result is ") + QString::number(v));
                f.setFinished();
            });

        auto job = baseJob.then<QString, QString>(
            [](const QString &v, KAsync::Future<QString> &f) {
                f.setValue(v.toUpper());
                f.setFinished();
            });
        KAsync::Future<QString> future = job.exec();
        QVERIFY(future.isFinished());
        QCOMPARE(future.value(), QString::fromLatin1("RESULT IS 42"));
    }
}

KAsync::Job<void> doStuff()
{
    return KAsync::wait(1);
}

void AsyncTest::testErrorHandling()
{
    //Failing job
    {
        auto future = KAsync::start<int>(
            [](KAsync::Future<int> &f) {
                f.setError({1, "error"});
            }).exec();
        QVERIFY(future.isFinished());
        QCOMPARE(future.errorCode(), 1);
        QCOMPARE(future.errorMessage().toUtf8(), QByteArray("error"));
    }

    //Call error handler
    {
        bool errorHandlerCalled = false;
        auto future = KAsync::error<int>({1, "error"})
            .then<int, int>([&errorHandlerCalled](const KAsync::Error &error, int) {
                errorHandlerCalled = true;
                COMPARERET(error.errorCode, 1, KAsync::error<int>(error));
                return KAsync::error<int>(error);
            }).exec();
        QVERIFY(future.isFinished());
        QVERIFY(errorHandlerCalled);
        QCOMPARE(future.errors().first(), KAsync::Error(1, "error"));
    } 

    //Propagate error
    {
        bool errorHandlerCalled = false;
        auto future = KAsync::error<int>({1, "error"})
        .then<int, int>(
            [](int) {
                VERIFYRET(false, KAsync::null<int>());
                return KAsync::null<int>();
            })
        .then<void, int>([&errorHandlerCalled](const KAsync::Error &error, int) {
                errorHandlerCalled = true;
                COMPARERET(error.errorCode, 1, KAsync::error<void>(error));
                return KAsync::error<void>(error);
            })
        .exec();

        QVERIFY(future.isFinished());
        QVERIFY(errorHandlerCalled);
        QCOMPARE(future.errors().first(), KAsync::Error(1, "error"));
    } 

    //Propagate error
    {
        bool errorHandlerCalled1 = false;
        bool errorHandlerCalled2 = false;
        auto future = KAsync::error<int>({1, "error"})
        .then<int, int>(
            [&errorHandlerCalled1](const KAsync::Error &error, int) {
                errorHandlerCalled1 = true;
                COMPARERET(error.errorCode, 1, KAsync::error<int>(error));
                return KAsync::error<int>(error);
            })
        .then<void, int>([&errorHandlerCalled2](const KAsync::Error &error, int) {
                errorHandlerCalled2 = true;
                COMPARERET(error.errorCode, 1, KAsync::error<void>(error));
                return KAsync::error<void>(error);
            })
        .exec();

        QVERIFY(future.isFinished());
        QVERIFY(errorHandlerCalled1);
        QVERIFY(errorHandlerCalled2);
        QCOMPARE(future.errors().first(), KAsync::Error(1, "error"));
    } 

    //Reconcile error
    {
        bool errorHandlerCalled1 = false;
        bool errorHandlerCalled2 = false;
        auto future = KAsync::error<int>({1, "error"})
        .then<int, int>(
            [&errorHandlerCalled1](const KAsync::Error &error, int) {
                errorHandlerCalled1 = true;
                COMPARERET(error, KAsync::Error(1, "error"), KAsync::null<int>());
                return KAsync::null<int>();
            })
        .then<void, int>([&errorHandlerCalled2](const KAsync::Error &error, int) {
                VERIFYRET(!error, KAsync::null<void>());
                errorHandlerCalled2 = true;
                return KAsync::null<void>();
            })
        .exec();

        QVERIFY(errorHandlerCalled1);
        QVERIFY(errorHandlerCalled2);
        QVERIFY(future.isFinished());
        QVERIFY(!future.hasError());
    } 

    //Propagate value on error
    {
        KAsync::Future<int> future = KAsync::value<int>(1)
        .onError([](const KAsync::Error &error) {
                Q_UNUSED(error);
                QVERIFY(false);
            })
        .exec();

        QVERIFY(future.isFinished());
        QCOMPARE(future.value(), 1);
    }

    //Ensure an error continuation is called and can clear the error
    {
        bool errorHandlerCalled1 = false;
        auto job = KAsync::null()
            .then(KAsync::error({1, "error"}));

        auto future = job.then([&](const KAsync::Error &error) {
                    errorHandlerCalled1 = true;
                    COMPARERET(error, KAsync::Error(1, "error"), KAsync::null());
                    return KAsync::null();
                })
        .exec();
        QVERIFY(future.isFinished());
        QVERIFY(errorHandlerCalled1);
        QCOMPARE(future.errorCode(), 0);
    }
    //Ensure an error continuation is called and can clear the error in a nested job
    {
        bool errorHandlerCalled1 = false;
        bool continuationCalled = false;
        auto error = KAsync::Error(1, "error");
        auto job = KAsync::error<void>(error)
            .then([&] (const KAsync::Error &e){
                return doStuff()
                    .then([] {
                    })
                    .then([e, &continuationCalled] {
                        continuationCalled = true;
                        return KAsync::error(e);
                    });
            });

        auto job2 = job.then([&](const KAsync::Error &e) {
                    errorHandlerCalled1 = true;
                    COMPARERET(e, error, KAsync::null());
                    return KAsync::null();
                });
        auto future = job2.exec();
        future.waitForFinished();
        QVERIFY(future.isFinished());
        QVERIFY(errorHandlerCalled1);
        QVERIFY(continuationCalled);
        QCOMPARE(future.errorCode(), 0);
    }
}

void AsyncTest::testContext()
{

    QWeakPointer<QObject> refToObj;
    {
        KAsync::Job<int> job = KAsync::null<int>();
        {
            auto contextObject = QSharedPointer<QObject>::create();
            refToObj = contextObject.toWeakRef();
            QVERIFY(refToObj);
            job = KAsync::start<int>(
                [](KAsync::Future<int> &future) {
                    new AsyncSimulator<int>(future, 42);
                });
            job.addToContext(contextObject);

            //Ensure the context survives for the whole duration of the job
            job = job.then<int>([](KAsync::Future<int> &future) {
                    new AsyncSimulator<int>(future, 42);
                });
        }

        QVERIFY(refToObj);

        {
            //Ensure the context survives copies
            auto job2 = job;
            job = KAsync::null<int>();
            KAsync::Future<int> future = job2.exec();
            QVERIFY(refToObj);
            future.waitForFinished();
        }
    }
    QVERIFY(!refToObj);
}

void AsyncTest::testGuard()
{
    //Guard before
    {
        auto guard = new QObject;
        bool continuationCalled = false;
        auto job = KAsync::null<void>();
        job = KAsync::start<void>(
            [](KAsync::Future<void> &future) {
                new AsyncSimulator<void>(future);
            });
        job.guard(guard);
        job = job
            .then([] {})
            .then([&] {
                continuationCalled = true;
                qWarning() << "Continuation called";
            });
        auto future = job.exec();
        qWarning() << "deleting guard";
        delete guard;
        QVERIFY(!continuationCalled);
        future.waitForFinished();
        QVERIFY(!continuationCalled);
    }
    //Guard after
    {
        auto guard = new QObject;
        bool continuationCalled = false;
        auto job = KAsync::null<void>();
        job = KAsync::start<void>(
            [](KAsync::Future<void> &future) {
                new AsyncSimulator<void>(future);
            });
        job = job.then([&] {
                continuationCalled = true;
                qWarning() << "Continuation called";
            })
            .then([] {})
            .guard(guard);
        auto future = job.exec();
        qWarning() << "deleting guard";
        delete guard;
        QVERIFY(!continuationCalled);
        future.waitForFinished();
        QVERIFY(!continuationCalled);
    }


    {
        auto guard = new QObject;
        bool continuationCalled = false;
        auto job = KAsync::null<void>();
        job = KAsync::start<void>(
            [](KAsync::Future<void> &future) {
                new AsyncSimulator<void>(future);
            });
        //Ensure the continuation is never called.
        job = job.then([&] {
                continuationCalled = true;
            }).guard(guard);
        //Ensure the guard survives copies
        auto job2 = job;
        job = KAsync::null<void>();
        auto future = job2.exec();
        delete guard;
        QVERIFY(!continuationCalled);
        future.waitForFinished();
        QVERIFY(!continuationCalled);
    }
}

void AsyncTest::testDoWhile()
{
    int i = 0;
    auto future = KAsync::doWhile([&i]() {
        i++;
        if (i < 5) {
            return KAsync::value(KAsync::Continue);
        }
        return KAsync::value(KAsync::Break);
    })
    .exec();
    future.waitForFinished();
    QVERIFY(future.isFinished());
    QCOMPARE(i, 5);
}

void AsyncTest::testDoWhileWithJob()
{
    int i = 0;
    auto future = KAsync::doWhile(KAsync::start<KAsync::ControlFlowFlag>([&i]() {
        i++;
        if (i < 5) {
            return KAsync::Continue;
        }
        return KAsync::Break;
    }))
    .exec();
    future.waitForFinished();
    QVERIFY(future.isFinished());
    QCOMPARE(i, 5);
}

void AsyncTest::testNestedDoWhile()
{
    int outer = 0;
    int inner = 0;
    int total = 0;
    auto future = KAsync::doWhile(
        KAsync::start([&] {
            outer++;
            inner = 0;
            //Safety net to avoid infinite recursion
            Q_ASSERT(outer < 3);
        }).then(KAsync::doWhile(KAsync::start<KAsync::ControlFlowFlag>([&] {
                total++;
                inner++;
                //This is the abort condition of the outer loop
                if (inner < 2) {
                    return KAsync::Continue;
                }
                return KAsync::Break;
            })
        ))
        .then([&] {
            //This is the abort condition of the outer loop
            if (outer < 2) {
                return KAsync::Continue;
            }
            return KAsync::Break;
        })
    )
    .exec();
    future.waitForFinished();
    QVERIFY(future.isFinished());
    QCOMPARE(outer, 2);
    QCOMPARE(total, 4);
}

void AsyncTest::testAsyncPromises()
{
    auto job = KAsync::start<int>(
        [](KAsync::Future<int> &future) {
            new AsyncSimulator<int>(future, 42);
        });

    KAsync::Future<int> future = job.exec();

    future.waitForFinished();
    QCOMPARE(future.value(), 42);
}

void AsyncTest::testNestedAsync()
{
    bool done = false;

    auto job = KAsync::start<int>(
        [](KAsync::Future<int> &future) {
            auto innerJob = KAsync::start<int>([](KAsync::Future<int> &innerFuture) {
                new AsyncSimulator<int>(innerFuture, 42);
            }).then<void>([&future](KAsync::Future<void> &innerThenFuture) {
                future.setFinished();
                innerThenFuture.setFinished();
            });
            innerJob.exec().waitForFinished();
        }
    ).then<int, int>([&done](int result, KAsync::Future<int> &future) {
        done = true;
        future.setValue(result);
        future.setFinished();
    });
    job.exec();

    QTRY_VERIFY(done);
}

void AsyncTest::testVoidNestedJob()
{
    bool innerDone1 = false;
    bool innerDone2 = false;
    bool innerDone3 = false;
    auto job = KAsync::start<void>(
        [&innerDone1]() -> KAsync::Job<void> {
            return KAsync::start<void>([&innerDone1]() {
                innerDone1 = true;
                return KAsync::null<void>();
            });
        }
    )
    .then<void>([&innerDone2, &innerDone3]() -> KAsync::Job<void> {
        return KAsync::start<void>([&innerDone2]() {
            innerDone2 = true;
            return KAsync::null<void>();
        })
        .then<void>([&innerDone3]() {
            innerDone3 = true;
            return KAsync::null<void>();
        });
    });
    auto future = job.exec();
    future.waitForFinished();
    QCOMPARE(future.errorCode(), 0);
    QVERIFY(innerDone1);
    QVERIFY(innerDone2);
    QVERIFY(innerDone3);
}

void AsyncTest::testAsyncEach()
{
    {
        auto job = KAsync::value<std::vector<int>>({1});
        auto future = job.each<void>([](int i) {
                Q_UNUSED(i);
                return KAsync::null<void>();
            }).exec();
        QVERIFY(future.isFinished());
    }

    const QList<int> expected({1, 2, 3});

    auto job = KAsync::value<QList<int>>({1, 2, 3});
    {
        QList<int> result;
        //This is the all manual version
        auto subjob = KAsync::forEach<QList<int>>(
                KAsync::start<void, int>([&result](int i) {
                    result << i;
                    return KAsync::null<void>();
                })
            );
        auto future = job.then<void, QList<int>>(
                subjob
            ).exec();
        future.waitForFinished();
        QVERIFY(future.isFinished());
        QCOMPARE(result, expected);

    }
    {
        QList<int> result;
        //An this is the convenience wrapper
        auto future = job.each([&result](int i) {
                result << i;
                return KAsync::null<void>();
            }).exec();
        future.waitForFinished();
        QVERIFY(future.isFinished());
        QCOMPARE(result, expected);
    }
}

void AsyncTest::testAsyncSerialEach()
{
    {
        auto job = KAsync::value<std::vector<int>>({1});
        auto future = job.serialEach<void>([](int i) {
                Q_UNUSED(i);
                return KAsync::null<void>();
            }).exec();

    }

    const QList<int> expected({1, 2, 3});

    auto job = KAsync::value<QList<int>>({1, 2, 3});
    {
        QList<int> result;
        auto subjob = KAsync::serialForEach<QList<int>>(
                KAsync::start<void, int>([&](int i) {
                    result << i;
                    return KAsync::null<void>();
                })
            );
        auto future = job.then<void, QList<int>>(subjob).exec();
        future.waitForFinished();
        QVERIFY(future.isFinished());
        QCOMPARE(result, expected);
    }
    {
        QList<int> result;
        //An this is the convenience wrapper
        auto future = job.serialEach([&result](int i) {
                result << i;
                return KAsync::null<void>();
            }).exec();
        future.waitForFinished();
        QVERIFY(future.isFinished());
        QCOMPARE(result, expected);
    }
}

void AsyncTest::benchmarkSyncThenExecutor()
{
    auto job = KAsync::start<int>(
        []() {
            return 1;
        });

    QBENCHMARK {
       job.exec();
    }
}

void AsyncTest::benchmarkFutureThenExecutor()
{
    auto job = KAsync::start<int>(
        [](KAsync::Future<int> &f) {
            f.setResult(1);
        });

    QBENCHMARK {
       job.exec();
    }
}

void AsyncTest::benchmarkThenExecutor()
{
    // auto job = KAsync::start<int>(
    //     []() {
    //         return KAsync::value(1);
    //     });
    
    //This is exactly the same as the future version (due to it's implementation).
    auto job = KAsync::value(1);

    QBENCHMARK {
       job.exec();
    }
}

//Ensure we don't have to define the template arguments
void AsyncTest::noTemplateArguments()
{
    double input = 42;
    auto job1 = KAsync::start<int, double>(
        [](double i)  {
            return KAsync::value<int>(i);
        });

    //Async continuation
    {
        auto job2 = job1.then([](int value) {
            return KAsync::value(QString::number(value));
        });
        static_assert(std::is_same<decltype(job2), KAsync::Job<QString, double>>::value, "Not the same type");
        auto future = job2.exec(input);
        QVERIFY(future.isFinished());
        QCOMPARE(future.value(), QString::number(42));
    }

    //void async continuation
    {
        auto job2 = job1.then([](int) {
            return KAsync::null<void>();
        });
        // using foo = decltype(job2)::foo;
        static_assert(std::is_same<decltype(job2), KAsync::Job<void, double>>::value, "Not the same type");
        auto future = job2.exec(input);
        QVERIFY(future.isFinished());
        // QCOMPARE(future.value(), QString::number(42));
    }

    //Job continuation
    {
        int value = input;
        auto job2 = job1.then(KAsync::value(QString::number(value)));
        static_assert(std::is_same<decltype(job2), KAsync::Job<QString, double>>::value, "Not the same type");
        auto future = job2.exec(input);
        QVERIFY(future.isFinished());
        QCOMPARE(future.value(), QString::number(42));
    }

    //Sync continuation
    {
        auto job2 = job1.then([](int value) {
            return QString::number(value);
        });
        static_assert(std::is_same<decltype(job2), KAsync::Job<QString, double>>::value, "Not the same type");
        auto future = job2.exec(input);
        QVERIFY(future.isFinished());
        QCOMPARE(future.value(), QString::number(42));
    }

    //void sync continuation
    {
        auto job2 = job1.then([](int) {
        });
        static_assert(std::is_same<decltype(job2), KAsync::Job<void, double>>::value, "Not the same type");
        auto future = job2.exec(input);
        QVERIFY(future.isFinished());
    }

    //The following examples should result in a compile time error
    {
    //Should fail due to no argument
    // auto job3 = job1.then([]() {
    //         return KAsync::value(QString::number(42));
    // });

    //Should fail due to wrong argument
    // auto job3 = job1.then([](QByteArray foo) {
    //         eturn KAsync::value<QString>(QString::number(42));
    // });
    }
}

void AsyncTest::testValueJob()
{
    QList<QByteArray> list;
    list << "foo";
    list << "foo2";
    auto job1 = KAsync::null();
    {
        auto job = job1.then(KAsync::value(list));
        static_assert(std::is_same<decltype(job), KAsync::Job<QList<QByteArray>>>::value, "Not the same type");
        auto future = job.exec();
        QCOMPARE(future.value(), list);
    }
    {
        auto job = job1.then([&] { return KAsync::value(list); });
        static_assert(std::is_same<decltype(job), KAsync::Job<QList<QByteArray>>>::value, "Not the same type");
        auto future = job.exec();
        QCOMPARE(future.value(), list);
    }
    {
        auto job = job1.then([&] { return list; });
        static_assert(std::is_same<decltype(job), KAsync::Job<QList<QByteArray>>>::value, "Not the same type");
        auto future = job.exec();
        QCOMPARE(future.value(), list);
    }
}

QTEST_MAIN(AsyncTest)

#include "asynctest.moc"
