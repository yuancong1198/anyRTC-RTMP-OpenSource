/*
 *  Copyright 2004 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

/*
WebRTC中有两类消息需要在消息循环中得以处理，即时消息Message以及延迟消息DelayedMessage。
    他们被投递进入消息循环时，分别进入不同的队列，即时消息Message进入MessageLits类别的即时消息队列，
该队列是先入先出的对列，这类消息期待得到立即的处理；延迟消息DelayedMessage进入PriorityQueue类别的延迟消息队列
，该队列是优先级队列，根据延迟消息本身的触发时间以及消息序号进行排序，越早触发的消息将越早得以处理。
如果再算上线程上同步发送消息，同步阻塞执行方法的话还有另外一个SendList，当然，这不是本文需要说明的内容了。
    消息数据的类别有好多种，各自起到不同的作用，尤其要注意DisposeData用来利用消息循环处理消息的功能来自然而然地销毁某个类别的数据。
    消息处理器最重要的就是其OnMessage方法，该方法是消息最终得以处理的地方。
WebRTC中的很多重要的类就是MessageHandler的子类，比如PeerConnection；
    消息处理器的子类FunctorMessageHandler为跨线程执行方法提供了便利，后续会在线程相关的文章中重点阐述。
*/

#ifndef WEBRTC_BASE_MESSAGEQUEUE_H_
#define WEBRTC_BASE_MESSAGEQUEUE_H_

#include <string.h>

#include <algorithm>
#include <list>
#include <memory>
#include <queue>
#include <vector>

#include "webrtc/base/basictypes.h"
#include "webrtc/base/constructormagic.h"
#include "webrtc/base/criticalsection.h"
#include "webrtc/base/location.h"
#include "webrtc/base/messagehandler.h"
#include "webrtc/base/scoped_ref_ptr.h"
#include "webrtc/base/sharedexclusivelock.h"
#include "webrtc/base/sigslot.h"
#include "webrtc/base/socketserver.h"
#include "webrtc/base/timeutils.h"
#include "webrtc/base/thread_annotations.h"

namespace rtc {

struct Message;
class MessageQueue;

// MessageQueueManager does cleanup of of message queues

//MessageQueueManager类是为了对MessageQueue进行管理而存在
class MessageQueueManager {
 public:
  static void Add(MessageQueue *message_queue);
  static void Remove(MessageQueue *message_queue);
  static void Clear(MessageHandler *handler);

  // For testing purposes, we expose whether or not the MessageQueueManager
  // instance has been initialized. It has no other use relative to the rest of
  // the functions of this class, which auto-initialize the underlying
  // MessageQueueManager instance when necessary.
  static bool IsInitialized();

  // Mainly for testing purposes, for use with a simulated clock.
  // Ensures that all message queues have processed delayed messages
  // up until the current point in time.
  static void ProcessAllMessageQueues();

 private:
  static MessageQueueManager* Instance();

  MessageQueueManager();
  ~MessageQueueManager();

  void AddInternal(MessageQueue *message_queue);
  void RemoveInternal(MessageQueue *message_queue);
  void ClearInternal(MessageHandler *handler);
  void ProcessAllMessageQueuesInternal();

  static MessageQueueManager* instance_;
  // This list contains all live MessageQueues.
  std::vector<MessageQueue *> message_queues_;
  //临界区类CriticalSection的对象，该成员保证多线程环境下访问安全
  CriticalSection crit_;
};

// Derive from this for specialized data
// App manages lifetime, except when messages are purged
// 定义了基类，并将析构函数定义为虚函数。
class MessageData {
 public:
  MessageData() {}
  virtual ~MessageData() {}
};

//使用模板定义的 MessageData 的一个子类，便于扩展。
template <class T>
class TypedMessageData : public MessageData {
 public:
  explicit TypedMessageData(const T& data) : data_(data) { }
  const T& data() const { return data_; }
  T& data() { return data_; }
 private:
  T data_;
};

// Like TypedMessageData, but for pointers that require a delete.
//类似于 TypedMessageData，用于指针类型。在析构函数中，自动对该指针调用 delete。
template <class T>
class ScopedMessageData : public MessageData {
 public:
  explicit ScopedMessageData(T* data) : data_(data) { }
  const std::unique_ptr<T>& data() const { return data_; }
  std::unique_ptr<T>& data() { return data_; }

 private:
  std::unique_ptr<T> data_;
};

// Like ScopedMessageData, but for reference counted pointers.
//类似于 TypedMessageData，用于引用计数的指针类型。
template <class T>
class ScopedRefMessageData : public MessageData {
 public:
  explicit ScopedRefMessageData(T* data) : data_(data) { }
  const scoped_refptr<T>& data() const { return data_; }
  scoped_refptr<T>& data() { return data_; }
 private:
  scoped_refptr<T> data_;
};

//模板函数，便于创建 TypedMessageData
template<class T>
inline MessageData* WrapMessageData(const T& data) {
  return new TypedMessageData<T>(data);
}

template<class T>
inline const T& UseMessageData(MessageData* data) {
  return static_cast< TypedMessageData<T>* >(data)->data();
}

//这是一个很特殊的消息，用以将某个对象交给消息引擎销毁。
template<class T>
class DisposeData : public MessageData {
 public:
  explicit DisposeData(T* data) : data_(data) { }
  virtual ~DisposeData() { delete data_; }
 private:
  T* data_;
};

const uint32_t MQID_ANY = static_cast<uint32_t>(-1);
const uint32_t MQID_DISPOSE = static_cast<uint32_t>(-2);

// No destructor

//WebRTC中消息相关的类分为两种，一种是Message即时消息，投放到消息循环中期待能被立马消费；
struct Message {
  Message()
      : phandler(nullptr), message_id(0), pdata(nullptr), ts_sensitive(0) {}
  inline bool Match(MessageHandler* handler, uint32_t id) const {
    return (handler == NULL || handler == phandler)
           && (id == MQID_ANY || id == message_id);
  }
  Location posted_from;   //posted_from代表了消息的来源，来源于那个方法、哪个文件哪一行；
  MessageHandler *phandler; //主要使用该成员MessageHandler的OnMessage(Message* msg)来对消息进行处理；
  uint32_t message_id;
  MessageData *pdata;
  int64_t ts_sensitive;//64位时间戳，单位ms。当不关心消息是否处理过慢时，也即消息时间不敏感时，该值为0；若关心消息是否得到即时处理，一般会设置ts_sensitive为消息创建时的时间戳 + kMaxMsgLatency常量(150ms)，当该消息从消息循环中取出被处理时，将会检测当前时间msCurrent与ts_sensitive的大小，若msCurrent>ts_sensitive，则表示该消息并没有得到即时的处理，会打印警告日志。超时时间计算为msCurrent-ts_sensitive+kMaxMsgLatency。
};

typedef std::list<Message> MessageList;

// DelayedMessage goes into a priority queue, sorted by trigger time.  Messages
// with the same trigger time are processed in num_ (FIFO) order.
//另外一种是DelayedMessage延迟消息，投放到消息循环中不会立马被消费，而是延迟一段时间才会被消费
class DelayedMessage {
 public:
  DelayedMessage(int64_t delay,
                 int64_t trigger,
                 uint32_t num,
                 const Message& msg)
      : cmsDelay_(delay), msTrigger_(trigger), num_(num), msg_(msg) {}

  bool operator< (const DelayedMessage& dmsg) const {
    return (dmsg.msTrigger_ < msTrigger_)
           || ((dmsg.msTrigger_ == msTrigger_) && (dmsg.num_ < num_));
  }

  int64_t cmsDelay_;  // for debugging 延迟多久触发消息，仅作调试使用
  int64_t msTrigger_; //触发消息的时间
  uint32_t num_;    //添加消息的时间
  Message msg_;  //消息本身
};
//注意：在使用延迟消息时，不需要自行构建 DelayedMessage 实例。直接调用 MessageQueue::PostDelayed 或者 MessageQueue::PostAt 函数即可。


/*
MessageQueue提供了两方面的功能，消息循环中的消息队列功能以及通过持有SocketServer对象带来的IO多路复用功能。
在MessageQueue内部这两部分功能不是完全孤立的，而是相互配合在一起使用。
尤其是在MessageQueue的核心方法Get()中体现得淋漓尽致。
1.实现了消息一个完整地消息队列，该队列包括立即执行消息队列、延迟执行消息队列和具有优先级的消息队列。
2.MessageQueue 类也是 Thread 类的基类。
3.所有的 WebRTC 的线程都是支持消息队列的。
*/
class MessageQueue {
 public:
  static const int kForever = -1;

  // Create a new MessageQueue and optionally assign it to the passed
  // SocketServer. Subclasses that override Clear should pass false for
  // init_queue and call DoInit() from their constructor to prevent races
  // with the MessageQueueManager using the object while the vtable is still
  // being created.
  MessageQueue(SocketServer* ss, bool init_queue);
  MessageQueue(std::unique_ptr<SocketServer> ss, bool init_queue);

  // NOTE: SUBCLASSES OF MessageQueue THAT OVERRIDE Clear MUST CALL
  // DoDestroy() IN THEIR DESTRUCTORS! This is required to avoid a data race
  // between the destructor modifying the vtable, and the MessageQueueManager
  // calling Clear on the object from a different thread.
  virtual ~MessageQueue();

  SocketServer* socketserver();
  void set_socketserver(SocketServer* ss);

  // Note: The behavior of MessageQueue has changed.  When a MQ is stopped,
  // futher Posts and Sends will fail.  However, any pending Sends and *ready*
  // Posts (as opposed to unexpired delayed Posts) will be delivered before
  // Get (or Peek) returns false.  By guaranteeing delivery of those messages,
  // we eliminate the race condition when an MessageHandler and MessageQueue
  // may be destroyed independently of each other.
  virtual void Quit();
  virtual bool IsQuitting();
  virtual void Restart();

  // Get() will process I/O until:
  //  1) A message is available (returns true)
  //  2) cmsWait seconds have elapsed (returns false)
  //  3) Stop() is called (returns false)
  /*
    MessageQueue::Get：等待接收消息。
    pmsg：存放消息的指针，用于返回消息
    cmsWait：等待的时间，kForever表示无限等待
    process_io：是否要求 SocketServer处理IO信号
  */
  //消息获取:
  virtual bool Get(Message *pmsg, int cmsWait = kForever,
                   bool process_io = true);
  virtual bool Peek(Message *pmsg, int cmsWait = 0);
  //消息投递:
  virtual void Post(const Location& posted_from,
                    MessageHandler* phandler,
                    uint32_t id = 0,
                    MessageData* pdata = NULL,
                    bool time_sensitive = false);
  /*
    MessageQueue::PostDelayed：发送一个延迟消息（从当前时间计算延迟处理时间）
    cmsDelay：延迟毫秒数
    phandler：消息处理器（ MessageHandler 的子类）
    id：消息ID
    pdata：MessageData指针
  */
  virtual void PostDelayed(const Location& posted_from,
                           int cmsDelay,
                           MessageHandler* phandler,
                           uint32_t id = 0,
                           MessageData* pdata = NULL);
  /*
    MessageQueue::PostAt：发送一个延迟消息（直接指定延迟处理时间）
    tstamp：消息触发的时间
    phandler：消息处理器（ MessageHandler的子类）
    id：消息ID
    pdata：MessageData指针
  */
  virtual void PostAt(const Location& posted_from,
                      int64_t tstamp,
                      MessageHandler* phandler,
                      uint32_t id = 0,
                      MessageData* pdata = NULL);
  // TODO(honghaiz): Remove this when all the dependencies are removed.
  virtual void PostAt(const Location& posted_from,
                      uint32_t tstamp,
                      MessageHandler* phandler,
                      uint32_t id = 0,
                      MessageData* pdata = NULL);
  /*
    MessageQueue::Clear：通过指定 MessageHandler和消息ID删除消息
    phandler：指定被删除消息的 MessageHandler
    id：指定被删除消息的ID；如果该id为MQID_ANY所有与phandler相关的消息都将被删除
    removed：返回所有被删除消息的列表
  */
  //消息清理:
  virtual void Clear(MessageHandler* phandler,
                     uint32_t id = MQID_ANY,
                     MessageList* removed = NULL);
  //消息处理:
  virtual void Dispatch(Message *pmsg);
  virtual void ReceiveSends();

  // Amount of time until the next message can be retrieved
  //从现在到下一条将要触发的延迟消息的触发时间的毫秒数（无参数）
  virtual int GetDelay();

  bool empty() const { return size() == 0u; }
  size_t size() const {
    CritScope cs(&crit_);  // msgq_.size() is not thread safe.
    return msgq_.size() + dmsgq_.size() + (fPeekKeep_ ? 1u : 0u);
  }

  // Internally posts a message which causes the doomed object to be deleted
  //请求消息引擎删除一个对象（delete对象指针）
  //销毁消息:如果想要销毁某个对象，而不方便立马销毁，那么就可以将调用消息循环的Dispose()方法让消息循环帮忙进行数据销毁。
  template<class T> void Dispose(T* doomed) {
    if (doomed) {
      Post(RTC_FROM_HERE, NULL, MQID_DISPOSE, new DisposeData<T>(doomed));
    }
  }

  // When this signal is sent out, any references to this queue should
  // no longer be used.
  //通知接收者（observer）消息队列将要删除（参数无）
  sigslot::signal0<> SignalQueueDestroyed;

 protected:
  class PriorityQueue : public std::priority_queue<DelayedMessage> {
   public:
    container_type& container() { return c; }
    void reheap() { make_heap(c.begin(), c.end(), comp); }
  };

  void DoDelayPost(const Location& posted_from,
                   int64_t cmsDelay,
                   int64_t tstamp,
                   MessageHandler* phandler,
                   uint32_t id,
                   MessageData* pdata);

  // Perform initialization, subclasses must call this from their constructor
  // if false was passed as init_queue to the MessageQueue constructor.
  void DoInit();

  // Perform cleanup, subclasses that override Clear must call this from the
  // destructor.
  void DoDestroy();

  void WakeUpSocketServer();

  bool fPeekKeep_; //指示是否存在一个被Peek出来的消息；
  //消息循环相关：MQ中有3个地方存储了消息：
  Message msgPeek_; //存储被Peek出来的一个即时消息；    
  MessageList msgq_ GUARDED_BY(crit_);  //消息队列，队列内的消息按照先进先出的原则立即执行
  PriorityQueue dmsgq_ GUARDED_BY(crit_); //延迟消息队列，队列内的消息按照指定的时间延迟执行
  uint32_t dmsgq_next_num_ GUARDED_BY(crit_);//下一个延迟消息的序号，单调递增；
  CriticalSection crit_;
  //MQ状态指示：
  bool fStop_;  //指示MQ是否已经Quit，即停止工作，不继续接受处理消息；
  bool fInitialized_; //指示MQ已经被初始化，即已经被添加到MQM的管理队列中；
  bool fDestroyed_; //指示MQ已经被销毁，即已经被MQM移除，并且MQ将立马被析构；

 private:
  // The SocketServer might not be owned by MessageQueue.
  //IO多路复用相关：
  SocketServer* ss_ GUARDED_BY(ss_lock_);//持有的SocketServer类，用以完成IO多路复用操作；
  // Used if SocketServer ownership lies with |this|.
  std::unique_ptr<SocketServer> own_ss_;//与ss_一样，只是经过转移语句，使得该SocketServer对象只由该MQ持有。
  SharedExclusiveLock ss_lock_;         

  RTC_DISALLOW_IMPLICIT_CONSTRUCTORS(MessageQueue);
};

}  // namespace rtc

#endif  // WEBRTC_BASE_MESSAGEQUEUE_H_
