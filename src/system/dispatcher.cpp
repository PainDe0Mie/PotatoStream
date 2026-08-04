#include "dispatcher.hpp"
#include <algorithm>

MessageDispatcher::MessageDispatcher() {
    for (uint8_t i = 0; i < MessageType::MESSAGE_TYPE_COUNT; i++) {
        subscribers[static_cast<MessageType>(i)] = std::vector<ISubscriber *>();
    }
}

void MessageDispatcher::subscribe(MessageType type, ISubscriber *sub) {
    if (sub == nullptr) {
        return;
    }

    subscriber_lock.lock();
    std::vector<ISubscriber *> &sub_list = subscribers[type];
    auto sub_pos = std::find(sub_list.begin(), sub_list.end(), sub);
    // Prevent duplication
    if (sub_pos == sub_list.end()) {
        subscribers[type].push_back(sub);
    }
    subscriber_lock.unlock();
}

void MessageDispatcher::unsubscribe(MessageType type, ISubscriber *sub) {
    if (sub == nullptr) {
        return;
    }

    subscriber_lock.lock();
    std::vector<ISubscriber *> &sub_list = subscribers[type];
    auto sub_pos = std::find(sub_list.begin(), sub_list.end(), sub);
    if (sub_pos != sub_list.end()) {
        sub_list.erase(sub_pos);
    }
    subscriber_lock.unlock();
}

void MessageDispatcher::post_immediate(std::shared_ptr<IMessage> m) {
    // Snapshot the subscriber list under the lock, then release.
    // This prevents:
    //   1. Deadlock if a subscriber calls post/subscribe/unsubscribe
    //      from within accept() (reentrancy).
    //   2. Iterator invalidation if a subscriber modifies the list
    //      during dispatch.
    //   3. Long lock hold times while accept() runs.
    subscriber_lock.lock();
    std::vector<ISubscriber *> snapshot = subscribers[m->getMessageType()];
    subscriber_lock.unlock();

    for (ISubscriber *sub : snapshot) {
        if (sub == nullptr) {
            continue;
        }
        sub->accept(m.get());
    }
}

void MessageDispatcher::post(std::shared_ptr<IMessage> m) {
    message_lock.lock();
    message_queue.push(m);
    message_lock.unlock();
}

void MessageDispatcher::dispatch_all() {
    // Single lock per message (not two like the original _is_queue_empty + pop)
    while (true) {
        std::shared_ptr<IMessage> m;
        message_lock.lock();
        if (message_queue.empty()) {
            message_lock.unlock();
            break;
        }
        m = message_queue.front();
        message_queue.pop();
        message_lock.unlock();

        post_immediate(m);
    }
}
