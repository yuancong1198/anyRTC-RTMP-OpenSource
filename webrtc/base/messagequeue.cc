/*
 *  Copyright 2004 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */
#include <algorithm>

#include "webrtc/base/atomicops.h"
#include "webrtc/base/checks.h"
#include "webrtc/base/common.h"
#include "webrtc/base/logging.h"
#include "webrtc/base/messagequeue.h"
#include "webrtc/base/stringencode.h"
#include "webrtc/base/thread.h"
#include "webrtc/base/trace_event.h"

namespace rtc {

const int kMaxMsgLatency = 150;  // 150 ms
const int kSlowDispatchLoggingThreshold = 50;  // 50 ms

//------------------------------------------------------------------
// MessageQueueManager

MessageQueueManager* MessageQueueManager::instance_ = NULL;

MessageQueueManager* MessageQueueManager::Instance() {
  // Note: This is not thread safe, but it is first called before threads are
  // spawned.
  if (!instance_)
    instance_ = new MessageQueueManager;
  return instance_;
}

bool MessageQueueManager::IsInitialized() {
  return instance_ != NULL;
}

MessageQueueManager::MessageQueueManager() {}

MessageQueueManager::~MessageQueueManager() {
}

void MessageQueueManager::Add(MessageQueue *message_queue) {
  return Instance()->AddInternal(message_queue);
}
void MessageQueueManager::AddInternal(MessageQueue *message_queue) {
  // MessageQueueManager methods should be non-reentrant, so we
  // ASSERT that is the case.  If any of these ASSERT, please
  // contact bpm or jbeda.
#if CS_DEBUG_CHECKS  // CurrentThreadIsOwner returns true by default.
  ASSERT(!crit_.CurrentThreadIsOwner());
#endif
  CritScope cs(&crit_);
  message_queues_.push_back(message_queue);
}

void MessageQueueManager::Remove(MessageQueue *message_queue) {
  // If there isn't a message queue manager instance, then there isn't a queue
  // to remove.
  if (!instance_) return;
  return Instance()->RemoveInternal(message_queue);
}
void MessageQueueManager::RemoveInternal(MessageQueue *message_queue) {
#if CS_DEBUG_CHECKS  // CurrentThreadIsOwner returns true by default.
  ASSERT(!crit_.CurrentThreadIsOwner());  // See note above.
#endif
  // If this is the last MessageQueue, destroy the manager as well so that
  // we don't leak this object at program shutdown. As mentioned above, this is
  // not thread-safe, but this should only happen at program termination (when
  // the ThreadManager is destroyed, and threads are no longer active).
  bool destroy = false;
  {
    CritScope cs(&crit_);
    std::vector<MessageQueue *>::iterator iter;
    iter = std::find(message_queues_.begin(), message_queues_.end(),
                     message_queue);
    if (iter != message_queues_.end()) {
      message_queues_.erase(iter);
    }
    destroy = message_queues_.empty();
  }
  if (destroy) {
    instance_ = NULL;
    delete this;
  }
}

void MessageQueueManager::Clear(MessageHandler *handler) {
  // If there isn't a message queue manager instance, then there aren't any
  // queues to remove this handler from.
  if (!instance_) return;
  return Instance()->ClearInternal(handler);
}
void MessageQueueManager::ClearInternal(MessageHandler *handler) {
#if CS_DEBUG_CHECKS  // CurrentThreadIsOwner returns true by default.
  ASSERT(!crit_.CurrentThreadIsOwner());  // See note above.
#endif
  CritScope cs(&crit_);
  std::vector<MessageQueue *>::iterator iter;
  for (iter = message_queues_.begin(); iter != message_queues_.end(); iter++)
    (*iter)->Clear(handler);
}

void MessageQueueManager::ProcessAllMessageQueues() {
  if (!instance_) {
    return;
  }
  return Instance()->ProcessAllMessageQueuesInternal();
}

void MessageQueueManager::ProcessAllMessageQueuesInternal() {
#if CS_DEBUG_CHECKS  // CurrentThreadIsOwner returns true by default.
  ASSERT(!crit_.CurrentThreadIsOwner());  // See note above.
#endif
  // Post a delayed message at the current time and wait for it to be dispatched
  // on all queues, which will ensure that all messages that came before it were
  // also dispatched.
  volatile int queues_not_done;
  auto functor = [&queues_not_done] { AtomicOps::Decrement(&queues_not_done); };
  FunctorMessageHandler<void, decltype(functor)> handler(functor);
  {
    CritScope cs(&crit_);
    queues_not_done = static_cast<int>(message_queues_.size());
    //线程调用消息处理时会遍历所有的message_queues_，然后向每个MessageQueue中投递一个消息ID为MQID_DISPOSE的延迟消息
    for (MessageQueue* queue : message_queues_) {
      queue->PostDelayed(RTC_FROM_HERE, 0, &handler);
    }
  }
  // Note: One of the message queues may have been on this thread, which is why
  // we can't synchronously wait for queues_not_done to go to 0; we need to
  // process messages as well.
  //获取调用该方法的线程所关联的Thread对象，并通过Thread对象的ProcessMessages方法不断的从当前线程的MQ中取出消息进行处理，直到queues_not_done为0
  while (AtomicOps::AcquireLoad(&queues_not_done) > 0) {
    rtc::Thread::Current()->ProcessMessages(0);
  }
}

//------------------------------------------------------------------
// MessageQueue
/*
    0.初始化所有的成员
    1.断言ss不能传空指针，将MQ的指针传递给ss使得二者相互持有相互访问
    2.将MQ加入到MQM的管理指针，fInitialized_标志置为true，指示该MQ已经初始化完成。
*/
MessageQueue::MessageQueue(SocketServer* ss, bool init_queue)
    : fStop_(false), fPeekKeep_(false),
      dmsgq_next_num_(0), fInitialized_(false), fDestroyed_(false), ss_(ss) {
  RTC_DCHECK(ss);
  // Currently, MessageQueue holds a socket server, and is the base class for
  // Thread.  It seems like it makes more sense for Thread to hold the socket
  // server, and provide it to the MessageQueue, since the Thread controls
  // the I/O model, and MQ is agnostic to those details.  Anyway, this causes
  // messagequeue_unittest to depend on network libraries... yuck.
  ss_->SetMessageQueue(this);
  if (init_queue) {
    DoInit();
  }
}

MessageQueue::MessageQueue(std::unique_ptr<SocketServer> ss, bool init_queue)
    : MessageQueue(ss.get(), init_queue) {
  own_ss_ = std::move(ss);
}

MessageQueue::~MessageQueue() {
  DoDestroy();
}

void MessageQueue::DoInit() {
  if (fInitialized_) {
    return;
  }

  fInitialized_ = true;
  MessageQueueManager::Add(this);
}

/*
    0.设置fDestroyed_为true，表示MQ被销毁
    1.发送信号SignalQueueDestroyed()告知关注了MQ的对象不要再访问该MQ了
    2.从MQM中移除自己
    3.清理MQ中的所有消息
    4.从ss中移除MQ的指针
*/
void MessageQueue::DoDestroy() {
  if (fDestroyed_) {
    return;
  }

  fDestroyed_ = true;
  // The signal is done from here to ensure
  // that it always gets called when the queue
  // is going away.
  SignalQueueDestroyed();
  MessageQueueManager::Remove(this);
  Clear(NULL);

  SharedScope ss(&ss_lock_);
  if (ss_) {
    ss_->SetMessageQueue(NULL);
  }
}

SocketServer* MessageQueue::socketserver() {
  SharedScope ss(&ss_lock_);
  return ss_;
}

void MessageQueue::set_socketserver(SocketServer* ss) {
  // Need to lock exclusively here to prevent simultaneous modifications from
  // other threads. Can't be a shared lock to prevent races with other reading
  // threads.
  // Other places that only read "ss_" can use a shared lock as simultaneous
  // read access is allowed.
  ExclusiveScope es(&ss_lock_);
  ss_ = ss ? ss : own_ss_.get();
  ss_->SetMessageQueue(this);
}

void MessageQueue::WakeUpSocketServer() {
  SharedScope ss(&ss_lock_);
  ss_->WakeUp();
}

/*
    MQ的运行状态由成员volatile int stop_来指示:
    0.当MQ停止运行后，MQ将不再接受处理Send，Post消息
    1.当MQ停止运行时，已经成功投递的消息仍将会被处理；
    2.为了确保上述这点，MessageHandler的销毁和MessageQueue的销毁是独立，分开的；
    3.并非所有的MQ都会处理消息，比如SignalThread线程。在这种情况下，为了确定投递的消息是否会被处理，应该使用IsProcessingMessagesForTesting()探知下
*/

void MessageQueue::Quit() {
  fStop_ = true;
  WakeUpSocketServer();
}

bool MessageQueue::IsQuitting() {
  return fStop_;
}

void MessageQueue::Restart() {
  fStop_ = false;
}

/*
简而言之就是查看之前是否已经Peek过一个MSG到成员msgPeek_中，若已经Peek过一个则直接将该消息返回；
若没有，则通过Get()方法从消息队列中取出一个消息，成功则将该消息交给msgPeek_成员，并将fPeekKeep_标志置为true。
*/
bool MessageQueue::Peek(Message *pmsg, int cmsWait) {
  // fPeekKeep_为真，表示已经Peek过一个MSG到msgPeek_直接将该MSG返回
  if (fPeekKeep_) {
    *pmsg = msgPeek_;
    return true;
  }
  // 若没有之前没有Peek过一个MSG
  if (!Get(pmsg, cmsWait))
    return false;
  //将Get到的消息放在msgPeek_中保存，并设置标志位
  msgPeek_ = *pmsg;
  fPeekKeep_ = true;
  return true;
}
/*
Get()方法会阻塞的处理IO，直到有消息可以处理 或者 cmsWait时间已经过去 或者 Stop()方法被调用。
*/
bool MessageQueue::Get(Message *pmsg, int cmsWait, bool process_io) {
  // Return and clear peek if present
  // Always return the peek if it exists so there is Peek/Get symmetry
  //是否存在一个Peek过的消息没有被处理？
  //0.优先处理该消息
  if (fPeekKeep_) {
    *pmsg = msgPeek_;
    fPeekKeep_ = false;
    return true;
  }

  // Get w/wait + timer scan / dispatch + socket / event multiplexer dispatch

  int64_t cmsTotal = cmsWait;
  int64_t cmsElapsed = 0;
  int64_t msStart = TimeMillis();
  int64_t msCurrent = msStart;
  while (true) {
    // Check for sent messages
    //1. 检查是否有send消息，若存在，先阻塞处理send消息
    ReceiveSends();

    // Check for posted events
    //// 检查所有post消息(即时消息+延迟消息)
    int64_t cmsDelayNext = kForever;
    bool first_pass = true;
    while (true) {
      // All queue operations need to be locked, but nothing else in this loop
      // (specifically handling disposed message) can happen inside the crit.
      // Otherwise, disposed MessageHandlers will cause deadlocks.
      // 上锁进行消息队列的访问
      {
        CritScope cs(&crit_);
        // On the first pass, check for delayed messages that have been
        // triggered and calculate the next trigger time.
        //2. 内部第一次循环，先检查延迟消息队列
        if (first_pass) {
          first_pass = false;
          // 将延迟消息队列dmsgq_中已经超过触发时间的消息全部取出放入到即时消息队列msgq_中
          // 计算当前时间距离下一个最早将要到达触发时间的消息还有多长时间cmsDelayNext。
          while (!dmsgq_.empty()) {
            if (msCurrent < dmsgq_.top().msTrigger_) {
              cmsDelayNext = TimeDiff(dmsgq_.top().msTrigger_, msCurrent);//检查延迟队列中的延迟消息是否已经到时
              break;
            }
            msgq_.push_back(dmsgq_.top().msg_);
            dmsgq_.pop();// 把到时的延时消息加入即时消息中
          }
        }
        // Pull a message off the message queue, if available.
        //3. 从即时消息队列msgq_队首取出第一个消息
        if (msgq_.empty()) {
          break;
        } else {
          *pmsg = msgq_.front();
          msgq_.pop_front();// 返回第一个即时消息
        }
      }  // crit_ is released here.

      // Log a warning for time-sensitive messages that we're late to deliver.
      // 如果消息对时间敏感，那么如果超过了最大忍耐时间kMaxMsgLatency才被处理
      // 则打印警告日志
      if (pmsg->ts_sensitive) {
        int64_t delay = TimeDiff(msCurrent, pmsg->ts_sensitive);
        if (delay > 0) {//如果敏感，并且已经过时，则打出警告日志
          LOG_F(LS_WARNING) << "id: " << pmsg->message_id << "  delay: "
                            << (delay + kMaxMsgLatency) << "ms";
        }
      }
      // If this was a dispose message, delete it and skip it.
      // 如果取出是需要销毁的消息，则销毁该消息，继续取下一个消息。
      if (MQID_DISPOSE == pmsg->message_id) {
        ASSERT(NULL == pmsg->phandler);
        delete pmsg->pdata;
        *pmsg = Message();
        continue;
      }
      return true;
    }
    // 走到这，说明当前没有消息要处理，很可能是处于Quit状态了，先判断一下
    if (fStop_)
      break;

    // Which is shorter, the delay wait or the asked wait?
    //4. 计算留给IO处理的时间
    int64_t cmsNext;
    if (cmsWait == kForever) {//Get无限期，那么距离下个延迟消息的时间就作为本次IO处理时间
      cmsNext = cmsDelayNext;
    } else { // Get有超时时间，计算本次IO处理时间
      cmsNext = std::max<int64_t>(0, cmsTotal - cmsElapsed);// 总体来说还剩多少时间
      if ((cmsDelayNext != kForever) && (cmsDelayNext < cmsNext))// 总体剩余时间和下一个延迟消息触发时间谁先到达？取其短者
        cmsNext = cmsDelayNext;
    }

    {
      // Wait and multiplex in the meantime
      //5. 阻塞处理IO多路复用 
      SharedScope ss(&ss_lock_);
      if (!ss_->Wait(static_cast<int>(cmsNext), process_io))
        return false;
    }

    // If the specified timeout expired, return
    //6. 计算是否所有时间都已耗尽，是否进入下一个大循环 
    msCurrent = TimeMillis();
    cmsElapsed = TimeDiff(msCurrent, msStart);
    if (cmsWait != kForever) {
      if (cmsElapsed >= cmsWait)
        return false;
    }
  }
  return false;
}

void MessageQueue::ReceiveSends() {
}

//主要就是将函数的入参封装成一个即时消息Message对象，然后放置到即时队列msgq_的队尾。
/*
0.如果消息循环已经处理停止状态，即stop_状态值不为0，那么消息循环拒绝消息入队，消息数据会被释放掉。此时，投递消息者是不知情的
1.如果消息对时间敏感，即想知道该消息是否即时被处理了，最大延迟不超过kMaxMsgLatency 150ms；
2.为了线程安全，队列的入队操作是需要加锁的，CritScope cs(&crit_)对象的构造和析构确保了这点；
3.消息入队后，由于处理消息是首要任务，因此，需要调用WakeUpSocketServer()使得IO多路复用的处理赶紧返回，即调用ss_->WakeUp()方法实现。
由于这块儿是IO多路复用实现内容，后续会专门写文章分析，此处只要知道该方法能够使得阻塞式的IO多路复用能结束阻塞，回到消息处理上来即可。
*/
void MessageQueue::Post(const Location& posted_from,
                        MessageHandler* phandler,
                        uint32_t id,
                        MessageData* pdata,
                        bool time_sensitive) {
  if (fStop_)
    return;

  // Keep thread safe
  // Add the message to the end of the queue
  // Signal for the multiplexer to return
  //Post的机制很简单　就是将传入的消息用stl链表保存起来 随后唤醒socket server
  {
    CritScope cs(&crit_);
    Message msg;
    msg.posted_from = posted_from;
    msg.phandler = phandler;
    msg.message_id = id;
    msg.pdata = pdata;
    if (time_sensitive) {
      msg.ts_sensitive = TimeMillis() + kMaxMsgLatency;
    }
    msgq_.push_back(msg);
  }
  WakeUpSocketServer();
}

void MessageQueue::PostDelayed(const Location& posted_from,
                               int cmsDelay,
                               MessageHandler* phandler,
                               uint32_t id,
                               MessageData* pdata) {
  return DoDelayPost(posted_from, cmsDelay, TimeAfter(cmsDelay), phandler, id,
                     pdata);
}

void MessageQueue::PostAt(const Location& posted_from,
                          uint32_t tstamp,
                          MessageHandler* phandler,
                          uint32_t id,
                          MessageData* pdata) {
  // This should work even if it is used (unexpectedly).
  int64_t delay = static_cast<uint32_t>(TimeMillis()) - tstamp;
  return DoDelayPost(posted_from, delay, tstamp, phandler, id, pdata);
}

void MessageQueue::PostAt(const Location& posted_from,
                          int64_t tstamp,
                          MessageHandler* phandler,
                          uint32_t id,
                          MessageData* pdata) {
  return DoDelayPost(posted_from, TimeUntil(tstamp), tstamp, phandler, id,
                     pdata);
}

void MessageQueue::DoDelayPost(const Location& posted_from,
                               int64_t cmsDelay,
                               int64_t tstamp,
                               MessageHandler* phandler,
                               uint32_t id,
                               MessageData* pdata) {
  if (fStop_) {
    return;
  }

  // Keep thread safe
  // Add to the priority queue. Gets sorted soonest first.
  // Signal for the multiplexer to return.

  {
    CritScope cs(&crit_);
    Message msg;
    msg.posted_from = posted_from;
    msg.phandler = phandler;
    msg.message_id = id;
    msg.pdata = pdata;
    DelayedMessage dmsg(cmsDelay, tstamp, dmsgq_next_num_, msg);
    dmsgq_.push(dmsg);
    // If this message queue processes 1 message every millisecond for 50 days,
    // we will wrap this number.  Even then, only messages with identical times
    // will be misordered, and then only briefly.  This is probably ok.
    VERIFY(0 != ++dmsgq_next_num_);
  }
  WakeUpSocketServer();
}

int MessageQueue::GetDelay() {
  CritScope cs(&crit_);

  if (!msgq_.empty())
    return 0;

  if (!dmsgq_.empty()) {
    int delay = TimeUntil(dmsgq_.top().msTrigger_);
    if (delay < 0)
      delay = 0;
    return delay;
  }

  return kForever;
}

void MessageQueue::Clear(MessageHandler* phandler,
                         uint32_t id,
                         MessageList* removed) {
  //0.线程安全，上锁处理；
  CritScope cs(&crit_);

  //1.MQ中消息可能存在的位置有3个；因此，需要从这3个地方去挨个查找能匹配的消息。
  //2.如果Clear()方法传入了一个MessageList* removed，匹配的消息都会进入该list；若是没有传入这样一个list，那么消息数据都将会立马销毁。
  // Remove messages with phandler
  //1.1Peek消息msgPeek_
  if (fPeekKeep_ && msgPeek_.Match(phandler, id)) {
    if (removed) {
      removed->push_back(msgPeek_);
    } else {
      delete msgPeek_.pdata;
    }
    fPeekKeep_ = false;
  }

  // Remove from ordered message queue
  //1.2即时消息队列msgq_
  for (MessageList::iterator it = msgq_.begin(); it != msgq_.end();) {
    if (it->Match(phandler, id)) {
      if (removed) {
        removed->push_back(*it);
      } else {
        delete it->pdata;
      }
      it = msgq_.erase(it);
    } else {
      ++it;
    }
  }

  // Remove from priority queue. Not directly iterable, so use this approach
  //1.3延迟消息队列dmsgq_
  PriorityQueue::container_type::iterator new_end = dmsgq_.container().begin();
  for (PriorityQueue::container_type::iterator it = new_end;
       it != dmsgq_.container().end(); ++it) {
    if (it->msg_.Match(phandler, id)) {
      if (removed) {
        removed->push_back(it->msg_);
      } else {
        delete it->msg_.pdata;
      }
    } else {
      *new_end++ = *it;
    }
  }
  dmsgq_.container().erase(new_end, dmsgq_.container().end());
  dmsgq_.reheap();
}

void MessageQueue::Dispatch(Message *pmsg) {
    //0.先打印一条trace日志；
  TRACE_EVENT2("webrtc", "MessageQueue::Dispatch", "src_file_and_line",
               pmsg->posted_from.file_and_line(), "src_func",
               pmsg->posted_from.function_name());
  //1.记录消息处理的开始时间start_time
  int64_t start_time = TimeMillis();
  //2.调用消息的MessageHandler的OnMessage方法进行消息处理
  pmsg->phandler->OnMessage(pmsg);
  //3.记录消息处理的结束时间end_tim
  int64_t end_time = TimeMillis();
  //4.计算消息处理花费了多长时间diff
  int64_t diff = TimeDiff(end_time, start_time);
  //5.如果消息花费时间过程，超过kSlowDispatchLoggingThreshold（50ms），则打印一条警告日志，告知从哪儿构建的消息花费了多长时间才消费完。
  if (diff >= kSlowDispatchLoggingThreshold) {
    LOG(LS_INFO) << "Message took " << diff << "ms to dispatch. Posted from: "
                 << pmsg->posted_from.ToString();
  }
}

}  // namespace rtc
