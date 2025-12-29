/*
 * Copyright 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// TODO(b/129481165): remove the #pragma below and fix conversion issues
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wconversion"

// #define LOG_NDEBUG 0

#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include "TransactionCallbackInvoker.h"
#include "BackgroundExecutor.h"
#include "Utils/FenceUtils.h"

#include <binder/IInterface.h>
#include <common/FlagManager.h>
#include <common/trace.h>
#include <utils/RefBase.h>

namespace android {

// Returns 0 if they are equal
//         <0 if the first id that doesn't match is lower in c2 or all ids match but c2 is shorter
//         >0 if the first id that doesn't match is greater in c2 or all ids match but c2 is longer
//
// See CallbackIdsHash for a explanation of why this works
static int compareCallbackIds(const std::vector<CallbackId>& c1,
                              const std::vector<CallbackId>& c2) {
    if (c1.empty()) {
        return !c2.empty();
    }
    return c1.front().id - c2.front().id;
}

static bool containsOnCommitCallbacks(const std::vector<CallbackId>& callbacks) {
    return !callbacks.empty() && callbacks.front().type == CallbackId::Type::ON_COMMIT;
}

void TransactionCallbackInvoker::addEmptyTransaction(const ListenerCallbacks& listenerCallbacks) {
    auto& [listener, callbackIds] = listenerCallbacks;
    mCompletedTransactions.emplace_back(listener, callbackIds);
}

void TransactionCallbackInvoker::addOnCommitCallbackHandles(std::vector<CallbackHandle>& handles) {
    auto it = std::remove_if(handles.begin(), handles.end(), [this](CallbackHandle& handle) {
        if (containsOnCommitCallbacks(handle.callbackIds)) {
            addCallbackHandle(std::move(handle));
            return true;
        }
        return false;
    });
    handles.erase(it, handles.end());
}

void TransactionCallbackInvoker::addCallbackHandles(std::vector<CallbackHandle>&& handles) {
    for (auto& handle : handles) {
        addCallbackHandle(std::move(handle));
    }
}

TransactionStats& TransactionCallbackInvoker::findOrCreateTransactionStats(
        const sp<IBinder>& listener, const std::vector<CallbackId>& callbackIds) {
    // Search back to front because the most recent transactions are at the back of the deque
    for (auto it = mCompletedTransactions.rbegin(); it != mCompletedTransactions.rend(); ++it) {
        auto& [listenerKey, transactionStats] = *it;
        if (listenerKey != listener) {
            continue;
        }
        if (compareCallbackIds(transactionStats.callbackIds, callbackIds) == 0) {
            return transactionStats;
        }
    }
    mCompletedTransactions.emplace_back(listener, callbackIds);
    return mCompletedTransactions.back().second;
}

void TransactionCallbackInvoker::addCallbackHandle(CallbackHandle&& handle) {
    TransactionStats& transactionStats =
            findOrCreateTransactionStats(handle.listener, handle.callbackIds);

    transactionStats.latchTime = handle.latchTime;
    // If the layer has already been destroyed, don't add the SurfaceControl to the callback.
    // The client side keeps a sp<> to the SurfaceControl so if the SurfaceControl has been
    // destroyed the client side is dead and there won't be anyone to send the callback to.
    sp<IBinder> surfaceControl = handle.surfaceControl.promote();
    if (!surfaceControl) {
        return;
    }

    FrameEventHistoryStats eventStats(handle.frameNumber, handle.previousFrameNumber,
                                      handle.gpuCompositionDoneFence->getSnapshot().fence,
                                      handle.compositorTiming, handle.refreshStartTime,
                                      handle.dequeueReadyTime);
    if (FlagManager::getInstance().fence_handling()) {
        if (handle.previousReleaseCallbackId != ReleaseCallbackId::INVALID_ID) {
            transactionStats.surfaceStats.emplace_back(surfaceControl, handle.acquireTimeOrFence,
                                                       Fence::NO_FENCE, handle.transformHint,
                                                       handle.currentMaxAcquiredBufferCount,
                                                       handle.cornerRadii, eventStats,
                                                       handle.previousReleaseCallbackId);
            mReleaseIdToFenceMerger.try_emplace(handle.previousReleaseCallbackId,
                                                 std::move(handle.fenceMerger));
            if (handle.bufferReleaseChannel) {
                mBufferReleases.emplace_back(std::move(handle.name),
                                             std::move(handle.bufferReleaseChannel),
                                             handle.previousReleaseCallbackId,
                                             sp<Fence>(),
                                             handle.currentMaxAcquiredBufferCount,
                                             FlagManager::getInstance().monitor_buffer_fences()
                                                     ? handle.previousBuffer
                                                     : std::weak_ptr<renderengine::ExternalTexture>());
            }
        }
    } else {
        sp<Fence> previousReleaseFence =
                handle.fenceMerger.waitAndGetFence(handle.name.c_str());
        transactionStats.surfaceStats.emplace_back(surfaceControl, handle.acquireTimeOrFence,
                                                   previousReleaseFence, handle.transformHint,
                                                   handle.currentMaxAcquiredBufferCount,
                                                   handle.cornerRadii, eventStats,
                                                   handle.previousReleaseCallbackId);
        if (handle.bufferReleaseChannel &&
            handle.previousReleaseCallbackId != ReleaseCallbackId::INVALID_ID) {
            if (FlagManager::getInstance().monitor_buffer_fences()) {
                if (auto previousBuffer = handle.previousBuffer.lock()) {
                    previousBuffer->getBuffer()
                            ->getDependencyMonitor()
                            .addEgress(FenceTime::makeValid(previousReleaseFence), "Txn release");
                }
            }
            mBufferReleases.emplace_back(std::move(handle.name),
                                         std::move(handle.bufferReleaseChannel),
                                         handle.previousReleaseCallbackId,
                                         std::move(previousReleaseFence),
                                         handle.currentMaxAcquiredBufferCount);
        }
    }
}

void TransactionCallbackInvoker::addPresentFence(sp<Fence> presentFence) {
    mPresentFence = std::move(presentFence);
}

void TransactionCallbackInvoker::sendCallbacks(bool onCommitOnly) {
    if (!FlagManager::getInstance().fence_handling()) {
        for (const auto& bufferRelease : mBufferReleases) {
            status_t status = bufferRelease.channel->writeReleaseFence(
                bufferRelease.callbackId, bufferRelease.fence,
                bufferRelease.currentMaxAcquiredBufferCount);
            if (status != OK) {
                ALOGE("[%s] writeReleaseFence failed. error %d (%s)",
                      bufferRelease.layerName.c_str(), -status, strerror(-status));
            }
        }
        mBufferReleases.clear();
    }

    // Build the set of ListenerStats to send. The listener stored in `listenerStatsToSend` comes
    // from the cross-process setTransactionState call to SF. This MUST be an
    // ITransactionCompletedListener. We keep it as an IBinder due to  consistency reasons: if we
    // interface_cast at the IPC boundary when reading a Parcel, we get pointers that compare
    // unequal in the SF process.
    ftl::SmallVector<ListenerStats, 10> listenerStatsToSend;

    auto findOrCreateListenerStats = [&](const sp<IBinder>& listener) -> ListenerStats& {
        auto it = std::find_if(listenerStatsToSend.begin(), listenerStatsToSend.end(),
                               [&](const ListenerStats& listenerStats) -> bool {
                                   return listenerStats.listener == listener;
                               });
        if (it != listenerStatsToSend.end()) {
            return *it;
        }
        ListenerStats& newStats = listenerStatsToSend.emplace_back();
        newStats.listener = listener;
        return newStats;
    };

    for (auto& [listener, transactionStats] : mCompletedTransactions) {
        // The `listener` pointer may be null if we've already sent this entry.
        // If the listener is no longer alive, there's nothing to do.
        if (listener == nullptr || !listener->isBinderAlive()) {
            continue;
        }

        if (onCommitOnly && !containsOnCommitCallbacks(transactionStats.callbackIds)) {
            continue;
        }

        // If the transaction has been latched
        if (transactionStats.latchTime >= 0 &&
            !containsOnCommitCallbacks(transactionStats.callbackIds)) {
            transactionStats.presentFence = mPresentFence;
        }

        // Move the transaction from completed to the callback.
        ListenerStats& listenerStats = findOrCreateListenerStats(listener);
        listenerStats.transactionStats.push_back(std::move(transactionStats));

        // Clear the listener pointer to indicate that we've sent these TransactionStats.
        listener.clear();
    }

    if (mPresentFence) {
        mPresentFence.clear();
    }

    if (listenerStatsToSend.empty()) {
        return;
    }

    if (FlagManager::getInstance().fence_handling()) {
        // Use a shared_ptr as FenceMerger is not copyable, but std::function requires the lambda
        // to be copyable.
        auto fenceMergers =
                std::make_shared<ftl::SmallMap<ReleaseCallbackId, FenceMerger, 20>>(
                        std::move(mReleaseIdToFenceMerger));
        BackgroundExecutor::getInstanceForTransaction().sendCallbacks(
                {[listenerStatsToSend = std::move(listenerStatsToSend),
                  fenceMergers = std::move(fenceMergers),
                  bufferReleases = std::move(mBufferReleases)]() mutable {
                    SFTRACE_NAME("TransactionCallbackInvoker::sendCallbacks");

                    for (auto& listenerStats : listenerStatsToSend) {
                        for (auto& transactionStats : listenerStats.transactionStats) {
                            for (auto& surfaceStats : transactionStats.surfaceStats) {
                                auto it = fenceMergers->find(
                                        surfaceStats.previousReleaseCallbackId);
                                if (it != fenceMergers->end()) {
                                    surfaceStats.previousReleaseFence =
                                            it->second.waitAndGetFence("merged_fence");
                                }
                            }
                        }
                    }

                    for (auto& bufferRelease : bufferReleases) {
                        sp<Fence> fence =
                                fenceMergers->find(bufferRelease.callbackId)
                                        ->second.waitAndGetFence("merged_fence");
                        if (FlagManager::getInstance().monitor_buffer_fences()) {
                            if (auto previousBuffer = bufferRelease.previousBuffer.lock()) {
                                previousBuffer->getBuffer()
                                        ->getDependencyMonitor()
                                        .addEgress(FenceTime::makeValid(fence), "Txn release");
                            }
                        }
                        status_t status =
                                bufferRelease.channel->writeReleaseFence(
                                    bufferRelease.callbackId, fence,
                                    bufferRelease.currentMaxAcquiredBufferCount);
                        if (status != OK) {
                            ALOGE("[%s] writeReleaseFence failed. error %d (%s)",
                                    bufferRelease.layerName.c_str(), -status, strerror(-status));
                        }
                    }

                    for (auto& stats : listenerStatsToSend) {
                        interface_cast<ITransactionCompletedListener>(stats.listener)
                                ->onTransactionCompleted(stats);
                    }
                }});
                mReleaseIdToFenceMerger.clear();
                mBufferReleases.clear();
    } else {
        BackgroundExecutor::getInstanceForTransaction().sendCallbacks(
                {[listenerStatsToSend = std::move(listenerStatsToSend)]() {
                    SFTRACE_NAME("TransactionCallbackInvoker::sendCallbacks");
                    for (auto& stats : listenerStatsToSend) {
                        interface_cast<ITransactionCompletedListener>(stats.listener)
                                ->onTransactionCompleted(stats);
                    }
                }});
    }
}

// -----------------------------------------------------------------------

CallbackHandle::CallbackHandle(const sp<IBinder>& transactionListener,
                               const std::vector<CallbackId>& ids, const sp<IBinder>& sc)
      : listener(transactionListener), callbackIds(ids), surfaceControl(sc) {}

} // namespace android

// TODO(b/129481165): remove the #pragma below and fix conversion issues
#pragma clang diagnostic pop // ignored "-Wconversion"
