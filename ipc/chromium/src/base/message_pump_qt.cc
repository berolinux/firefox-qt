// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma GCC visibility push(default)
#include <qabstracteventdispatcher.h>
#include <qevent.h>
#include <QCoreApplication>
#include <QThread>
#include <qtimer.h>
#pragma GCC visibility pop

#include "base/message_pump_qt.h"

#include <fcntl.h>
#include <limits>

#include "base/eintr_wrapper.h"
#include "base/logging.h"
#include "base/platform_thread.h"

namespace {
// Cached QEvent user type, registered for our event system
static int sPokeEvent;
}  // namespace

namespace base {

MessagePumpForUI::MessagePumpForUI() : state_(NULL), qt_pump(*this) {}

MessagePumpForUI::~MessagePumpForUI() {}

MessagePumpQt::MessagePumpQt(MessagePumpForUI& aPump)
    : pump(aPump), mTimer(new QTimer(this)) {
  // Register our custom event type, to use in qApp event loop
  sPokeEvent = QEvent::registerEventType();
  connect(mTimer, SIGNAL(timeout()), this, SLOT(dispatchDelayed()));
  mTimer->setSingleShot(true);
}

MessagePumpQt::~MessagePumpQt() {
  mTimer->stop();
  delete mTimer;
}

bool MessagePumpQt::event(QEvent* e) {
  if (e->type() == sPokeEvent) {
    pump.HandleDispatch();
    return true;
  }
  return false;
}

void MessagePumpQt::scheduleDelayedIfNeeded(
    const TimeTicks& delayed_work_time) {
  if (delayed_work_time.is_null()) {
    if (mTimer->isActive()) {
      mTimer->stop();
    }
    return;
  }

  if (mTimer->isActive()) {
    mTimer->stop();
  }

  TimeDelta later = delayed_work_time - TimeTicks::Now();
  // later.InMilliseconds() returns an int64_t, QTimer only accepts int's for
  // start(), std::min only works on exact same types.
  int64_t later64 = later.InMilliseconds();
  if (later64 <= 0) {
    // Overdue delayed work is handled by the Run() loop (DoDelayedWork sets
    // more_work_is_plausible). Do not start a 0ms QTimer or ScheduleWork()
    // here — both re-enter forever and peg the CPU.
    return;
  }
  int laterMsecs = later64 > std::numeric_limits<int>::max()
                       ? std::numeric_limits<int>::max()
                       : static_cast<int>(later64);
  mTimer->start(laterMsecs);
}

void MessagePumpQt::dispatchDelayed() { pump.HandleDispatch(); }

void MessagePumpForUI::Run(Delegate* delegate) {
  RunState state;
  state.delegate = delegate;
  state.should_quit = false;
  state.run_depth = state_ ? state_->run_depth + 1 : 1;
  // First iteration is non-blocking so RunAllPending() works. After that,
  // only Gecko work (DoWork/DoDelayedWork/DoIdleWork) keeps us non-blocking.
  // Do not treat processEvents()==true alone as more work — on Qt6 Wayland
  // that can stay true forever and busy-spin the CPU.
  bool more_work_is_plausible = true;

  RunState* previous_state = state_;
  state_ = &state;

  for (;;) {
    QAbstractEventDispatcher* dispatcher =
        QAbstractEventDispatcher::instance(QThread::currentThread());
    // An assertion seems too much here, as during startup,
    // the dispatcher might not be ready yet.
    if (!dispatcher) {
      return;
    }

    QEventLoop::ProcessEventsFlags flags = QEventLoop::AllEvents;
    if (!more_work_is_plausible) {
      flags |= QEventLoop::WaitForMoreEvents;
    }

    dispatcher->processEvents(flags);

    if (state_->should_quit) {
      break;
    }

    more_work_is_plausible = state_->delegate->DoWork();
    if (state_->should_quit) {
      break;
    }

    more_work_is_plausible |=
        state_->delegate->DoDelayedWork(&delayed_work_time_);
    if (state_->should_quit) {
      break;
    }

    qt_pump.scheduleDelayedIfNeeded(delayed_work_time_);

    if (more_work_is_plausible) {
      continue;
    }

    more_work_is_plausible = state_->delegate->DoIdleWork();
    if (state_->should_quit) {
      break;
    }
  }

  state_ = previous_state;
}

void MessagePumpForUI::HandleDispatch() {
  if (state_->should_quit) {
    return;
  }

  if (state_->delegate->DoWork()) {
    // there might be more, see more_work_is_plausible
    // variable above, that's why we ScheduleWork() to keep going.
    ScheduleWork();
  }

  if (state_->should_quit) {
    return;
  }

  state_->delegate->DoDelayedWork(&delayed_work_time_);
  qt_pump.scheduleDelayedIfNeeded(delayed_work_time_);
}

void MessagePumpForUI::Quit() {
  if (state_) {
    state_->should_quit = true;
  } else {
    NOTREACHED() << "Quit called outside Run!";
  }
}

void MessagePumpForUI::ScheduleWork() {
  QCoreApplication::postEvent(&qt_pump, new QEvent((QEvent::Type)sPokeEvent));
}

void MessagePumpForUI::ScheduleDelayedWork(const TimeTicks& delayed_work_time) {
  // On GLib implementation, a work source is defined which explicitly checks
  // the time that has passed. Here, on Qt we can use a QTimer that enqueues our
  // event signal in an event queue.
  delayed_work_time_ = delayed_work_time;
  qt_pump.scheduleDelayedIfNeeded(delayed_work_time_);
}

}  // namespace base
