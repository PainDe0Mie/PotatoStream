#pragma once

#include "ThreadLock.hpp"
#include "message.hpp"
#include "subscriber.hpp"
#include <map>
#include <memory>
#include <queue>
#include <vector>

class MessageDispatcher {
  public:
    MessageDispatcher();
    ~MessageDispatcher() = default;

    // NOTE: la static locale ci-dessous est initialisée de façon thread-safe
    // par le runtime C++11 (garantie standard sur les "magic statics"), sans
    // verrou manuel nécessaire. L'ancienne version testait/écrivait un
    // std::shared_ptr membre de classe SANS aucune synchronisation
    // ("if (instance == nullptr) instance = ...") : si deux threads
    // appelaient get_instance() pour la toute première fois à peu près en
    // même temps (input_loop / dispatch_loop / thread de rendu potato sur le
    // core 1 vs thread principal), les deux pouvaient voir "nullptr" et
    // construire+assigner le shared_ptr en parallèle -> écriture concurrente
    // non-atomique -> corruption possible du compteur de références ->
    // crash aléatoire selon l'ordonnancement des threads ce jour-là.
    static std::shared_ptr<MessageDispatcher> get_instance() {
        static std::shared_ptr<MessageDispatcher> instance =
            std::make_shared<MessageDispatcher>();
        return instance;
    }

    void subscribe(MessageType type, ISubscriber *sub);
    void unsubscribe(MessageType type, ISubscriber *sub);
    void post_immediate(std::shared_ptr<IMessage> m);
    void post(std::shared_ptr<IMessage> m);
    void dispatch_all();

  private:
    bool _is_queue_empty();

  private:
    std::map<MessageType, std::vector<ISubscriber *>> subscribers{};
    std::queue<std::shared_ptr<IMessage>> message_queue{};
    ThreadLock subscriber_lock;
    ThreadLock message_lock;
};
