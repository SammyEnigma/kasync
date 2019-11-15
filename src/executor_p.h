/*
 * Copyright 2014 - 2015 Daniel Vrátil <dvratil@redhat.com>
 * Copyright 2015 - 2019 Daniel Vrátil <dvratil@kde.org>
 * Copyright 2016  Christian Mollekopf <mollekopf@kolabsystems.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef KASYNC_EXECUTOR_P_H
#define KASYNC_EXECUTOR_P_H

#include "async_impl.h"
#include "execution_p.h"
#include "continuations_p.h"
#include "debug.h"

namespace KAsync {

template<typename T>
class Future;

template<typename T>
class FutureWatcher;

template<typename Out, typename ... In>
class ContinuationHolder;

template<typename Out, typename ... In>
class Job;

class Tracer;

namespace Private {

class ExecutorBase;
using ExecutorBasePtr = QSharedPointer<ExecutorBase>;

class ExecutorBase
{
    template<typename Out, typename ... In>
    friend class Executor;

    template<typename Out, typename ... In>
    friend class KAsync::Job;

    friend struct Execution;
    friend class KAsync::Tracer;

public:
    virtual ~ExecutorBase() = default;

    virtual ExecutionPtr exec(const ExecutorBasePtr &self, QSharedPointer<Private::ExecutionContext> context) = 0;

protected:
    ExecutorBase(const ExecutorBasePtr &parent)
        : mPrev(parent)
    {}

    template<typename T>
    KAsync::Future<T>* createFuture(const ExecutionPtr &execution) const
    {
        return new KAsync::Future<T>(execution);
    }

    void prepend(const ExecutorBasePtr &e)
    {
        if (mPrev) {
            mPrev->prepend(e);
        } else {
            mPrev = e;
        }
    }

    void addToContext(const QVariant &entry)
    {
        mContext.push_back(entry);
    }

    void guard(const QObject *o)
    {
        mGuards.push_back(QPointer<const QObject>{o});
    }

    QString mExecutorName;
    QVector<QVariant> mContext;
    QVector<QPointer<const QObject>> mGuards;
    ExecutorBasePtr mPrev;
};

template<typename Out, typename ... In>
class Executor : public ExecutorBase
{
    using PrevOut = typename detail::prevOut<In ...>::type;

public:
    explicit Executor(ContinuationHolder<Out, In ...> &&workerHelper, const ExecutorBasePtr &parent = {},
                      ExecutionFlag executionFlag = ExecutionFlag::GoodCase)
        : ExecutorBase(parent)
        , mContinuationHolder(std::move(workerHelper))
        , executionFlag(executionFlag)
    {
        STORE_EXECUTOR_NAME("Executor", Out, In ...);
    }

    virtual ~Executor() = default;

    void run(const ExecutionPtr &execution)
    {
        KAsync::Future<typename detail::prevOut<In ...>::type> *prevFuture = nullptr;
        if (execution->prevExecution) {
            prevFuture = execution->prevExecution->result<typename detail::prevOut<In ...>::type>();
            assert(prevFuture->isFinished());
        }

        //Execute one of the available workers
        KAsync::Future<Out> *future = execution->result<Out>();

        const auto &continuation = Executor<Out, In ...>::mContinuationHolder;
        if (continuationIs<AsyncContinuation<Out, In ...>>(continuation)) {
            continuationGet<AsyncContinuation<Out, In ...>>(continuation)(std::forward<In>(prevFuture->value()) ..., *future);
        } else if (continuationIs<AsyncErrorContinuation<Out, In ...>>(continuation)) {
            continuationGet<AsyncErrorContinuation<Out, In ...>>(continuation)(
                    prevFuture->hasError() ? prevFuture->errors().first() : Error(),
                    std::forward<In>(prevFuture->value()) ..., *future);
        } else if (continuationIs<SyncContinuation<Out, In ...>>(continuation)) {
            callAndApply(std::forward<In>(prevFuture->value()) ...,
                         continuationGet<SyncContinuation<Out, In ...>>(continuation), *future, std::is_void<Out>());
            future->setFinished();
        } else if (continuationIs<SyncErrorContinuation<Out, In ...>>(continuation)) {
            assert(prevFuture);
            callAndApply(prevFuture->hasError() ? prevFuture->errors().first() : Error(),
                         std::forward<In>(prevFuture->value()) ...,
                         continuationGet<SyncErrorContinuation<Out, In ...>>(continuation), *future, std::is_void<Out>());
            future->setFinished();
        } else if (continuationIs<JobContinuation<Out, In ...>>(continuation)) {
            executeJobAndApply(std::forward<In>(prevFuture->value()) ...,
                               continuationGet<JobContinuation<Out, In ...>>(continuation), *future, std::is_void<Out>());
        } else if (continuationIs<JobErrorContinuation<Out, In ...>>(continuation)) {
            executeJobAndApply(prevFuture->hasError() ? prevFuture->errors().first() : Error(),
                               std::forward<In>(prevFuture->value()) ...,
                               continuationGet<JobErrorContinuation<Out, In ...>>(continuation), *future, std::is_void<Out>());
        }

    }

    ExecutionPtr exec(const ExecutorBasePtr &self, QSharedPointer<Private::ExecutionContext> context) override
    {
        /*
         * One executor per job, created with the construction of the Job object.
         * One execution per job per exec(), created only once exec() is called.
         *
         * The executors make up the linked list that makes up the complete execution chain.
         *
         * The execution then tracks the execution of each executor.
         */

        // Passing 'self' to execution ensures that the Executor chain remains
        // valid until the entire execution is finished
        ExecutionPtr execution = ExecutionPtr::create(self);
#ifndef QT_NO_DEBUG
        execution->tracer = std::make_unique<Tracer>(execution.data()); // owned by execution
#endif

        context->guards += mGuards;

        // chainup
        execution->prevExecution = mPrev ? mPrev->exec(mPrev, context) : ExecutionPtr();

        execution->resultBase = ExecutorBase::createFuture<Out>(execution);
        //We watch our own future to finish the execution once we're done
        auto fw = new KAsync::FutureWatcher<Out>();
        QObject::connect(fw, &KAsync::FutureWatcher<Out>::futureReady,
                         [fw, execution]() {
                             execution->setFinished();
                             delete fw;
                         });
        fw->setFuture(*execution->result<Out>());

        KAsync::Future<PrevOut> *prevFuture = execution->prevExecution ? execution->prevExecution->result<PrevOut>()
                                                                       : nullptr;
        if (!prevFuture || prevFuture->isFinished()) { //The previous job is already done
            runExecution(prevFuture, execution, context->guardIsBroken());
        } else { //The previous job is still running and we have to wait for it's completion
            auto prevFutureWatcher = new KAsync::FutureWatcher<PrevOut>();
            QObject::connect(prevFutureWatcher, &KAsync::FutureWatcher<PrevOut>::futureReady,
                             [prevFutureWatcher, execution, this, context]() {
                                 auto prevFuture = prevFutureWatcher->future();
                                 assert(prevFuture.isFinished());
                                 delete prevFutureWatcher;
                                 runExecution(&prevFuture, execution, context->guardIsBroken());
                             });

            prevFutureWatcher->setFuture(*static_cast<KAsync::Future<PrevOut>*>(prevFuture));
        }

        return execution;
    }

private:
    void runExecution(KAsync::Future<PrevOut> *prevFuture, const ExecutionPtr &execution, bool guardIsBroken)
    {
        if (guardIsBroken) {
            execution->resultBase->setFinished();
            return;
        }
        if (prevFuture) {
            if (prevFuture->hasError() && executionFlag == ExecutionFlag::GoodCase) {
                //Propagate the error to the outer Future
                Q_ASSERT(prevFuture->errors().size() == 1);
                execution->resultBase->setError(prevFuture->errors().first());
                return;
            }
            if (!prevFuture->hasError() && executionFlag == ExecutionFlag::ErrorCase) {
                //Propagate the value to the outer Future
                KAsync::detail::copyFutureValue<PrevOut>(*prevFuture, *execution->result<PrevOut>());
                execution->resultBase->setFinished();
                return;
            }
        }
        run(execution);
    }

    void executeJobAndApply(In && ... input, const JobContinuation<Out, In ...> &func,
                            Future<Out> &future, std::false_type)
    {
        func(std::forward<In>(input) ...)
            .template then<void, Out>([&future](const KAsync::Error &error, Out &&v,
                                                KAsync::Future<void> &f) {
                if (error) {
                    future.setError(error);
                } else {
                    future.setResult(std::move(v));
                }
                f.setFinished();
            }).exec();
    }

    void executeJobAndApply(In && ... input, const JobContinuation<Out, In ...> &func,
                            Future<Out> &future, std::true_type)
    {
        func(std::forward<In>(input) ...)
            .template then<void>([&future](const KAsync::Error &error, KAsync::Future<void> &f) {
                if (error) {
                    future.setError(error);
                } else {
                    future.setFinished();
                }
                f.setFinished();
            }).exec();
    }

    void executeJobAndApply(const Error &error, In && ... input, const JobErrorContinuation<Out, In ...> &func,
                            Future<Out> &future, std::false_type)
    {
        func(error, std::forward<In>(input) ...)
            .template then<void, Out>([&future](const KAsync::Error &error, Out &&v,
                                                KAsync::Future<void> &f) {
                if (error) {
                    future.setError(error);
                } else {
                    future.setResult(std::move(v));
                }
                f.setFinished();
            }).exec();
    }

    void executeJobAndApply(const Error &error, In && ... input, const JobErrorContinuation<Out, In ...> &func,
                            Future<Out> &future, std::true_type)
    {
        func(error, std::forward<In>(input) ...)
            .template then<void>([&future](const KAsync::Error &error, KAsync::Future<void> &f) {
                if (error) {
                    future.setError(error);
                } else {
                    future.setFinished();
                }
                f.setFinished();
            }).exec();
    }

    void callAndApply(In && ... input, const SyncContinuation<Out, In ...> &func, Future<Out> &future, std::false_type)
    {
        future.setValue(func(std::forward<In>(input) ...));
    }

    void callAndApply(In && ... input, const SyncContinuation<Out, In ...> &func, Future<Out> &, std::true_type)
    {
        func(std::forward<In>(input) ...);
    }

    void callAndApply(const Error &error, In && ... input, const SyncErrorContinuation<Out, In ...> &func, Future<Out> &future, std::false_type)
    {
        future.setValue(func(error, std::forward<In>(input) ...));
    }

    void callAndApply(const Error &error, In && ... input, const SyncErrorContinuation<Out, In ...> &func, Future<Out> &, std::true_type)
    {
        func(error, std::forward<In>(input) ...);
    }

private:
    ContinuationHolder<Out, In ...> mContinuationHolder;
    const ExecutionFlag executionFlag;
};

} // namespace Private
} // nameapce KAsync

#endif
