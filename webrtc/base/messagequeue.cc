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
    //�̵߳�����Ϣ����ʱ��������е�message_queues_��Ȼ����ÿ��MessageQueue��Ͷ��һ����ϢIDΪMQID_DISPOSE���ӳ���Ϣ
    for (MessageQueue* queue : message_queues_) {
      queue->PostDelayed(RTC_FROM_HERE, 0, &handler);
    }
  }
  // Note: One of the message queues may have been on this thread, which is why
  // we can't synchronously wait for queues_not_done to go to 0; we need to
  // process messages as well.
  //��ȡ���ø÷������߳���������Thread���󣬲�ͨ��Thread�����ProcessMessages�������ϵĴӵ�ǰ�̵߳�MQ��ȡ����Ϣ���д���ֱ��queues_not_doneΪ0
  while (AtomicOps::AcquireLoad(&queues_not_done) > 0) {
    rtc::Thread::Current()->ProcessMessages(0);
  }
}

//------------------------------------------------------------------
// MessageQueue
/*
    0.��ʼ�����еĳ�Ա
    1.����ss���ܴ���ָ�룬��MQ��ָ�봫�ݸ�ssʹ�ö����໥�����໥����
    2.��MQ���뵽MQM�Ĺ���ָ�룬fInitialized_��־��Ϊtrue��ָʾ��MQ�Ѿ���ʼ����ɡ�
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
    0.����fDestroyed_Ϊtrue����ʾMQ������
    1.�����ź�SignalQueueDestroyed()��֪��ע��MQ�Ķ���Ҫ�ٷ��ʸ�MQ��
    2.��MQM���Ƴ��Լ�
    3.����MQ�е�������Ϣ
    4.��ss���Ƴ�MQ��ָ��
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
    MQ������״̬�ɳ�Աvolatile int stop_��ָʾ:
    0.��MQֹͣ���к�MQ�����ٽ��ܴ���Send��Post��Ϣ
    1.��MQֹͣ����ʱ���Ѿ��ɹ�Ͷ�ݵ���Ϣ�Խ��ᱻ����
    2.Ϊ��ȷ��������㣬MessageHandler�����ٺ�MessageQueue�������Ƕ������ֿ��ģ�
    3.�������е�MQ���ᴦ����Ϣ������SignalThread�̡߳�����������£�Ϊ��ȷ��Ͷ�ݵ���Ϣ�Ƿ�ᱻ����Ӧ��ʹ��IsProcessingMessagesForTesting()֪̽��
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
�����֮���ǲ鿴֮ǰ�Ƿ��Ѿ�Peek��һ��MSG����ԱmsgPeek_�У����Ѿ�Peek��һ����ֱ�ӽ�����Ϣ���أ�
��û�У���ͨ��Get()��������Ϣ������ȡ��һ����Ϣ���ɹ��򽫸���Ϣ����msgPeek_��Ա������fPeekKeep_��־��Ϊtrue��
*/
bool MessageQueue::Peek(Message *pmsg, int cmsWait) {
  // fPeekKeep_Ϊ�棬��ʾ�Ѿ�Peek��һ��MSG��msgPeek_ֱ�ӽ���MSG����
  if (fPeekKeep_) {
    *pmsg = msgPeek_;
    return true;
  }
  // ��û��֮ǰû��Peek��һ��MSG
  if (!Get(pmsg, cmsWait))
    return false;
  //��Get������Ϣ����msgPeek_�б��棬�����ñ�־λ
  msgPeek_ = *pmsg;
  fPeekKeep_ = true;
  return true;
}
/*
Get()�����������Ĵ���IO��ֱ������Ϣ���Դ��� ���� cmsWaitʱ���Ѿ���ȥ ���� Stop()���������á�
*/
bool MessageQueue::Get(Message *pmsg, int cmsWait, bool process_io) {
  // Return and clear peek if present
  // Always return the peek if it exists so there is Peek/Get symmetry
  //�Ƿ����һ��Peek������Ϣû�б�����
  //0.���ȴ������Ϣ
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
    //1. ����Ƿ���send��Ϣ�������ڣ�����������send��Ϣ
    ReceiveSends();

    // Check for posted events
    //// �������post��Ϣ(��ʱ��Ϣ+�ӳ���Ϣ)
    int64_t cmsDelayNext = kForever;
    bool first_pass = true;
    while (true) {
      // All queue operations need to be locked, but nothing else in this loop
      // (specifically handling disposed message) can happen inside the crit.
      // Otherwise, disposed MessageHandlers will cause deadlocks.
      // ����������Ϣ���еķ���
      {
        CritScope cs(&crit_);
        // On the first pass, check for delayed messages that have been
        // triggered and calculate the next trigger time.
        //2. �ڲ���һ��ѭ�����ȼ���ӳ���Ϣ����
        if (first_pass) {
          first_pass = false;
          // ���ӳ���Ϣ����dmsgq_���Ѿ���������ʱ�����Ϣȫ��ȡ�����뵽��ʱ��Ϣ����msgq_��
          // ���㵱ǰʱ�������һ�����罫Ҫ���ﴥ��ʱ�����Ϣ���ж೤ʱ��cmsDelayNext��
          while (!dmsgq_.empty()) {
            if (msCurrent < dmsgq_.top().msTrigger_) {
              cmsDelayNext = TimeDiff(dmsgq_.top().msTrigger_, msCurrent);//����ӳٶ����е��ӳ���Ϣ�Ƿ��Ѿ���ʱ
              break;
            }
            msgq_.push_back(dmsgq_.top().msg_);
            dmsgq_.pop();// �ѵ�ʱ����ʱ��Ϣ���뼴ʱ��Ϣ��
          }
        }
        // Pull a message off the message queue, if available.
        //3. �Ӽ�ʱ��Ϣ����msgq_����ȡ����һ����Ϣ
        if (msgq_.empty()) {
          break;
        } else {
          *pmsg = msgq_.front();
          msgq_.pop_front();// ���ص�һ����ʱ��Ϣ
        }
      }  // crit_ is released here.

      // Log a warning for time-sensitive messages that we're late to deliver.
      // �����Ϣ��ʱ�����У���ô����������������ʱ��kMaxMsgLatency�ű�����
      // ���ӡ������־
      if (pmsg->ts_sensitive) {
        int64_t delay = TimeDiff(msCurrent, pmsg->ts_sensitive);
        if (delay > 0) {//������У������Ѿ���ʱ������������־
          LOG_F(LS_WARNING) << "id: " << pmsg->message_id << "  delay: "
                            << (delay + kMaxMsgLatency) << "ms";
        }
      }
      // If this was a dispose message, delete it and skip it.
      // ���ȡ������Ҫ���ٵ���Ϣ�������ٸ���Ϣ������ȡ��һ����Ϣ��
      if (MQID_DISPOSE == pmsg->message_id) {
        ASSERT(NULL == pmsg->phandler);
        delete pmsg->pdata;
        *pmsg = Message();
        continue;
      }
      return true;
    }
    // �ߵ��⣬˵����ǰû����ϢҪ�����ܿ����Ǵ���Quit״̬�ˣ����ж�һ��
    if (fStop_)
      break;

    // Which is shorter, the delay wait or the asked wait?
    //4. ��������IO�����ʱ��
    int64_t cmsNext;
    if (cmsWait == kForever) {//Get�����ڣ���ô�����¸��ӳ���Ϣ��ʱ�����Ϊ����IO����ʱ��
      cmsNext = cmsDelayNext;
    } else { // Get�г�ʱʱ�䣬���㱾��IO����ʱ��
      cmsNext = std::max<int64_t>(0, cmsTotal - cmsElapsed);// ������˵��ʣ����ʱ��
      if ((cmsDelayNext != kForever) && (cmsDelayNext < cmsNext))// ����ʣ��ʱ�����һ���ӳ���Ϣ����ʱ��˭�ȵ��ȡ�����
        cmsNext = cmsDelayNext;
    }

    {
      // Wait and multiplex in the meantime
      //5. ��������IO��·���� 
      SharedScope ss(&ss_lock_);
      if (!ss_->Wait(static_cast<int>(cmsNext), process_io))
        return false;
    }

    // If the specified timeout expired, return
    //6. �����Ƿ�����ʱ�䶼�Ѻľ����Ƿ������һ����ѭ�� 
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

//��Ҫ���ǽ���������η�װ��һ����ʱ��ϢMessage����Ȼ����õ���ʱ����msgq_�Ķ�β��
/*
0.�����Ϣѭ���Ѿ�����ֹͣ״̬����stop_״ֵ̬��Ϊ0����ô��Ϣѭ���ܾ���Ϣ��ӣ���Ϣ���ݻᱻ�ͷŵ�����ʱ��Ͷ����Ϣ���ǲ�֪���
1.�����Ϣ��ʱ�����У�����֪������Ϣ�Ƿ�ʱ�������ˣ�����ӳٲ�����kMaxMsgLatency 150ms��
2.Ϊ���̰߳�ȫ�����е���Ӳ�������Ҫ�����ģ�CritScope cs(&crit_)����Ĺ��������ȷ������㣻
3.��Ϣ��Ӻ����ڴ�����Ϣ����Ҫ������ˣ���Ҫ����WakeUpSocketServer()ʹ��IO��·���õĴ���Ͻ����أ�������ss_->WakeUp()����ʵ�֡�
����������IO��·����ʵ�����ݣ�������ר��д���·������˴�ֻҪ֪���÷����ܹ�ʹ������ʽ��IO��·�����ܽ����������ص���Ϣ�����������ɡ�
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
  //Post�Ļ��ƺܼ򵥡����ǽ��������Ϣ��stl���������� �����socket server
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
  //0.�̰߳�ȫ����������
  CritScope cs(&crit_);

  //1.MQ����Ϣ���ܴ��ڵ�λ����3������ˣ���Ҫ����3���ط�ȥ����������ƥ�����Ϣ��
  //2.���Clear()����������һ��MessageList* removed��ƥ�����Ϣ��������list������û�д�������һ��list����ô��Ϣ���ݶ������������١�
  // Remove messages with phandler
  //1.1Peek��ϢmsgPeek_
  if (fPeekKeep_ && msgPeek_.Match(phandler, id)) {
    if (removed) {
      removed->push_back(msgPeek_);
    } else {
      delete msgPeek_.pdata;
    }
    fPeekKeep_ = false;
  }

  // Remove from ordered message queue
  //1.2��ʱ��Ϣ����msgq_
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
  //1.3�ӳ���Ϣ����dmsgq_
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
    //0.�ȴ�ӡһ��trace��־��
  TRACE_EVENT2("webrtc", "MessageQueue::Dispatch", "src_file_and_line",
               pmsg->posted_from.file_and_line(), "src_func",
               pmsg->posted_from.function_name());
  //1.��¼��Ϣ����Ŀ�ʼʱ��start_time
  int64_t start_time = TimeMillis();
  //2.������Ϣ��MessageHandler��OnMessage����������Ϣ����
  pmsg->phandler->OnMessage(pmsg);
  //3.��¼��Ϣ����Ľ���ʱ��end_tim
  int64_t end_time = TimeMillis();
  //4.������Ϣ�������˶೤ʱ��diff
  int64_t diff = TimeDiff(end_time, start_time);
  //5.�����Ϣ����ʱ����̣�����kSlowDispatchLoggingThreshold��50ms�������ӡһ��������־����֪���Ķ���������Ϣ�����˶೤ʱ��������ꡣ
  if (diff >= kSlowDispatchLoggingThreshold) {
    LOG(LS_INFO) << "Message took " << diff << "ms to dispatch. Posted from: "
                 << pmsg->posted_from.ToString();
  }
}

}  // namespace rtc
