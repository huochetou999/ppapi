// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ppapi/cpp/paint_manager.h"

#include "ppapi/c/pp_errors.h"
#include "ppapi/cpp/instance.h"
#include "ppapi/cpp/logging.h"
#include "ppapi/cpp/module.h"

namespace pp {

PaintManager::PaintManager()
    : instance_(NULL),
      client_(NULL),
      is_always_opaque_(false),
      callback_factory_(NULL) {
  // Set the callback object outside of the initializer list to avoid a
  // compiler warning about using "this" in an initializer list.
  callback_factory_.Initialize(this);
}

PaintManager::PaintManager(Instance* instance,
                           Client* client,
                           bool is_always_opaque)
    : instance_(instance),
      client_(client),
      is_always_opaque_(is_always_opaque),
      callback_factory_(NULL) {
  // Set the callback object outside of the initializer list to avoid a
  // compiler warning about using "this" in an initializer list.
  callback_factory_.Initialize(this);

  // You can not use a NULL client pointer.
  PP_DCHECK(client);
}

PaintManager::~PaintManager() {
}

void PaintManager::Initialize(Instance* instance,
                              Client* client,
                              bool is_always_opaque) {
  PP_DCHECK(!instance_ && !client_);  // Can't initialize twice.
  instance_ = instance;
  client_ = client;
  is_always_opaque_ = is_always_opaque;
}

void PaintManager::SetSize(const Size& new_size) {
  if (new_size == device_.size())
    return;

  device_ = DeviceContext2D(new_size, is_always_opaque_);
  if (device_.is_null())
    return;
  instance_->BindGraphicsDeviceContext(device_);

  manual_callback_pending_ = false;
  flush_pending_ = false;
  callback_factory_.CancelAll();

  Invalidate();
}

void PaintManager::Invalidate() {
  // You must call SetDevice before using.
  PP_DCHECK(!device_.is_null());

  EnsureCallbackPending();
  aggregator_.InvalidateRect(Rect(device_.size()));
}

void PaintManager::InvalidateRect(const Rect& rect) {
  // You must call SetDevice before using.
  PP_DCHECK(!device_.is_null());

  // Clip the rect to the device area.
  Rect clipped_rect = rect.Intersect(Rect(device_.size()));
  if (clipped_rect.IsEmpty())
    return;  // Nothing to do.

  EnsureCallbackPending();
  aggregator_.InvalidateRect(clipped_rect);
}

void PaintManager::ScrollRect(const Rect& clip_rect, const Point& amount) {
  // You must call SetDevice before using.
  PP_DCHECK(!device_.is_null());

  EnsureCallbackPending();
  aggregator_.ScrollRect(clip_rect, amount);
}

void PaintManager::EnsureCallbackPending() {
  // The best way for us to do the next update is to get a notification that
  // a previous one has completed. So if we're already waiting for one, we
  // don't have to do anything differently now.
  if (flush_pending_)
    return;

  // If no flush is pending, we need to do a manual call to get back to the
  // main thread. We may have one already pending, or we may need to schedule.
  if (manual_callback_pending_)
    return;

  Module::Get()->core()->CallOnMainThread(
      0,
      callback_factory_.NewCallback(&PaintManager::OnManualCallbackComplete),
      0);
  manual_callback_pending_ = true;
}

void PaintManager::DoPaint() {
  PP_DCHECK(aggregator_.HasPendingUpdate());

  // Make a copy of the pending update and clear the pending update flag before
  // actually painting. A plugin might cause invalidates in its Paint code, and
  // we want those to go to the *next* paint.
  PaintAggregator::PaintUpdate update = aggregator_.GetPendingUpdate();
  aggregator_.ClearPendingUpdate();

  // Apply any scroll before asking the client to paint.
  if (update.has_scroll)
    device_.Scroll(update.scroll_rect, update.scroll_delta);

  if (!client_->OnPaint(device_, update.paint_rects, update.paint_bounds))
    return;  // Nothing was painted, don't schedule a flush.

  int32_t result = device_.Flush(
      callback_factory_.NewCallback(&PaintManager::OnFlushComplete));

  // If you trigger this assertion, then your plugin has called Flush()
  // manually. When using the PaintManager, you should not call Flush, it will
  // handle that for you because it needs to know when it can do the next paint
  // by implementing the flush callback.
  //
  // Another possible cause of this assertion is re-using devices. If you
  // use one device, swap it with another, then swap it back, we won't know
  // that we've already scheduled a Flush on the first device. It's best to not
  // re-use devices in this way.
  PP_DCHECK(result != PP_ERROR_INPROGRESS);

  if (result == PP_ERROR_WOULDBLOCK) {
    flush_pending_ = true;
  } else {
    PP_DCHECK(result == PP_OK);  // Catch all other errors in debug mode.
  }
}

void PaintManager::OnFlushComplete(int32_t) {
  PP_DCHECK(flush_pending_);
  flush_pending_ = false;

  // If more paints were enqueued while we were waiting for the flush to
  // complete, execute them now.
  if (aggregator_.HasPendingUpdate())
    DoPaint();
}

void PaintManager::OnManualCallbackComplete(int32_t) {
  PP_DCHECK(manual_callback_pending_);
  manual_callback_pending_ = false;

  // Just because we have a manual callback doesn't mean there are actually any
  // invalid regions. Even though we only schedule this callback when something
  // is pending, a Flush callback could have come in before this callback was
  // executed and that could have cleared the queue.
  if (aggregator_.HasPendingUpdate())
    DoPaint();
}

}  // namespace pp