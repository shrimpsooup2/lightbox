#pragma once

#include <exception>
#include <functional>
#include <memory>
#include <utility>

#include <QCoreApplication>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QThreadPool>

// Runs slow work (file parsing, image decoding) off the UI thread and delivers the result back on it.
namespace lb::app {

// Runs `work` on QThreadPool::globalInstance(). Then, on the UI thread, calls `done(result)` or, if
// `work` threw, `failed(exception)`. Nothing is delivered if `context` was destroyed meanwhile.
// `work` must not touch the Scene or widgets. `Result` must be default-constructible and movable.
template <class Result>
void runInBackground(QObject* context, std::function<Result()> work,
                     std::function<void(Result)> done,
                     std::function<void(std::exception_ptr)> failed) {
    QPointer<QObject> guard(context);
    QThreadPool::globalInstance()->start([guard, work = std::move(work), done = std::move(done),
                                          failed = std::move(failed)]() mutable {
        auto result = std::make_shared<Result>();
        std::exception_ptr error;
        try {
            *result = work();
        } catch (...) {
            error = std::current_exception();
        }
        // Queued on the application object (always alive); `guard` is checked on the UI thread.
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [guard, result, error, done = std::move(done), failed = std::move(failed)]() mutable {
                if (!guard) {
                    return;
                }
                if (error) {
                    failed(error);
                } else {
                    done(std::move(*result));
                }
            },
            Qt::QueuedConnection);
    });
}

} // namespace lb::app
