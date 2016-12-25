//2011-11 Proyectos Equis Ka, s.l., jorge@jorgechamorro.com
//threads_a_gogo.cc

#include <v8.h>
#include <uv.h>
#include <node.h>
#include <node_version.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string>
#include <assert.h>

//using namespace node;
//using namespace v8;

//Macros BEGIN

#define kThreadMagicCookie 0x99c0ffee

#define TAGG_USE_LIBUV
#if (NODE_MAJOR_VERSION == 0) && (NODE_MINOR_VERSION <= 5)
  #undef TAGG_USE_LIBUV
#endif

#ifdef TAGG_USE_LIBUV
  #define WAKEUP_NODE_EVENT_LOOP uv_async_send(&thread->async_watcher);
#else
  #define WAKEUP_NODE_EVENT_LOOP ev_async_send(EV_DEFAULT_UC_ &thread->async_watcher);
#endif

//Macros END

//Type definitions BEGIN

typedef enum eventTypes {
  eventTypeNone = 0,
  eventTypeEmit,
  eventTypeEval,
  eventTypeLoad
} eventTypes;

struct emitStruct {
  int argc;
  char** argv;
  char* eventName;
};

struct evalStruct {
  int error;
  int hasCallback;
  union {
    char* resultado;
    char* scriptText;
  };
};

struct loadStruct {
  int error;
  int hasCallback;
  char* path;
};

struct eventsQueueItem {
  int eventType;
  eventsQueueItem* next;
  unsigned long serial;
  v8::Persistent<v8::Object> callback;
  union {
    emitStruct emit;
    evalStruct eval;
    loadStruct load;
  };
};

struct eventsQueue {
  eventsQueueItem* first;
  eventsQueueItem* pullPtr;
  union {
    eventsQueueItem* pushPtr;
    eventsQueueItem* last;
  };
};

typedef enum killTypes {
  kKillNicely= 1,
  kKillRudely
} killTypes;

typedef struct typeThread {

#ifdef TAGG_USE_LIBUV
  uv_async_t async_watcher; //MUST be the first one
#else
  ev_async async_watcher; //MUST be the first one
#endif
  
  long int id;
  pthread_t thread;
   int IDLE;
   int ended;
   int sigkill;
   int destroyed;
  int hasDestroyCallback;
  int hasIdleEventsListener;
  unsigned long threadMagicCookie;
  
  eventsQueue* threadEventsQueue;   //Jobs to run in the thread
  eventsQueue* processEventsQueue;  //Jobs to run in node's main thread
  
  pthread_cond_t idle_cv;
  pthread_mutex_t idle_mutex;
  
  v8::Isolate* isolate;
  v8::Persistent<v8::Object> nodeObject;
  v8::Persistent<v8::Object> nodeDispatchEvents;
  v8::Persistent<v8::Object> destroyCallback;
  
} typeThread;

//Type definitions END

//Prototypes BEGIN

static inline void beep (void);
static inline void qPush (eventsQueueItem*, eventsQueue*);
static inline eventsQueueItem* qPull (eventsQueue*);
static inline eventsQueueItem* qUsed (eventsQueue*);
static inline eventsQueueItem* nuQitem (eventsQueue*);
static eventsQueue* nuQueue (void);
static void qitemStorePush (eventsQueueItem*);
static eventsQueueItem* qitemStorePull (void);
static eventsQueue* qitemStoreInit (void);
static void destroyQueue (eventsQueue*);
static inline typeThread* isAThread (v8::Handle<v8::Object>);
static inline void wakeUpThread (typeThread*, int);
static v8::Handle<v8::Value> Puts (const v8::Arguments &);
static void* threadBootProc (void*);
static inline char* o2cstr (v8::Handle<v8::Value>);
static void eventLoop (typeThread*);
static void notifyIdle (typeThread*);
static void cleanUpAfterThread (typeThread*);
static void Callback (
#ifdef TAGG_USE_LIBUV
  uv_async_t*
#else
  EV_P_ ev_async*
#endif
                           , int);
static v8::Handle<v8::Value> Destroy (const v8::Arguments &);
static v8::Handle<v8::Value> Eval (const v8::Arguments &);
static v8::Handle<v8::Value> Load (const v8::Arguments &);
static inline void pushEmitEvent (eventsQueue*, const v8::Arguments &);
static v8::Handle<v8::Value> processEmit (const v8::Arguments &);
static v8::Handle<v8::Value> threadEmit (const v8::Arguments &);
static v8::Handle<v8::Value> Create (const v8::Arguments &);
void Init (v8::Handle<v8::Object>);

//Prototypes END


//Globals BEGIN

const char* k_TAGG_VERSION= "0.1.12";

static int TAGG_DEBUG= 0;
static bool useLocker;
static long int threadsCtr= 0;
static v8::Persistent<v8::Object> boot_js;
static v8::Persistent<v8::String> id_symbol;
static v8::Persistent<v8::String> version_symbol;
static v8::Persistent<v8::ObjectTemplate> threadTemplate;
static eventsQueue* qitemStore;
static unsigned long serial= 0;

#include "boot.js.c"
#include "pool.js.c"

//Globals END

/*

cd deps/minifier/src
gcc minify.c -o minify
cat ../../../src/events.js | ./minify kEvents_js > ../../../src/kEvents_js
cat ../../../src/load.js | ./minify kLoad_js > ../../../src/kLoad_js
cat ../../../src/createPool.js | ./minify kCreatePool_js > ../../../src/kCreatePool_js
cat ../../../src/nextTick.js | ./minify kNextTick_js > ../../../src/kNextTick_js

//node-waf configure uninstall distclean configure build install

*/








//jejeje
static inline void beep (void) {
  printf("\a"), fflush (stdout);  // que es lo mismo que \x07
}







//Se puede usar en cualquier thread pero solo si pasas la cola apropiada
static inline void qPush (eventsQueueItem* qitem, eventsQueue* queue) {
  TAGG_DEBUG && printf("Q_PUSH\n");
  qitem->next= NULL;
  assert(queue->pushPtr != NULL);
  assert(queue->pushPtr->next == NULL);
  queue->pushPtr->next= qitem;
  queue->pushPtr= qitem;
}







//Se puede usar en cualquier thread pero solo si pasas la cola apropiada
static eventsQueueItem* qPull (eventsQueue* queue) {
  TAGG_DEBUG && printf("Q_PULL\n");
  eventsQueueItem* qitem= queue->pullPtr;
  assert(qitem != NULL);
  while ((qitem->eventType == eventTypeNone) && qitem->next) {
    qitem= qitem->next;
    queue->pullPtr= qitem;
  }
  if (qitem->eventType == eventTypeNone)
    return NULL;
  else
    return qitem;
}







//Se puede usar en cualquier thread pero solo si pasas la cola apropiada
static inline eventsQueueItem* qUsed (eventsQueue* queue) {
  TAGG_DEBUG && printf("Q_USED\n");
  eventsQueueItem* qitem= NULL;
  assert(queue->first != NULL);
  assert(queue->pullPtr != NULL);
  if (queue->first != queue->pullPtr) {
    qitem= queue->first;
    assert(qitem->next != NULL);
    assert(queue->first != queue->pullPtr);
    queue->first= qitem->next;
    qitem->next= NULL;
  }
  return qitem;
}







//Se puede usar en cualquier thread pero solo si pasas la cola apropiada
static inline eventsQueueItem* nuQitem (eventsQueue* queue) {
  TAGG_DEBUG && printf("Q_NU_Q_ITEM\n");
  eventsQueueItem* qitem= NULL;
  if (queue) qitem= qUsed(queue);
  if (!qitem) {
    qitem= (eventsQueueItem*) calloc(1, sizeof(eventsQueueItem));
    //beep();
  }
  qitem->serial= serial++;
  qitem->eventType= eventTypeNone;
  qitem->next= NULL;
  return qitem;
}







//Sólo se debe usar en main/node's thread !
static eventsQueue* nuQueue (void) {
  TAGG_DEBUG && printf("Q_NU_QUEUE\n");
  eventsQueue* queue= (eventsQueue*) calloc(1, sizeof(eventsQueue));
  eventsQueueItem* qitem= qitemStorePull();
  if (!qitem) qitem= nuQitem(NULL);
  queue->first= qitem;
  qitem->eventType= eventTypeNone;
  int i= 96;
  while (--i) {
    qitem->next= qitemStorePull();
    if (!qitem->next) qitem->next= nuQitem(NULL);
    (qitem= qitem->next)->eventType= eventTypeNone;
  }
  qitem->next= NULL;
  queue->pullPtr= queue->pushPtr= qitem;
  return queue;
}







//Sólo se debe usar en main/node's thread !
static void qitemStorePush (eventsQueueItem* qitem) {
  TAGG_DEBUG && printf("Q_ITEM_STORE_PUSH\n");
  qitem->next= NULL;
  assert(qitemStore->last != NULL);
  assert(qitemStore->last->next == NULL);
  qitemStore->last->next= qitem;
  qitemStore->last= qitem;
}







//Sólo se debe usar en main/node's thread !
static eventsQueueItem* qitemStorePull (void) {
  TAGG_DEBUG && printf("Q_ITEM_STORE_PULL\n");
  eventsQueueItem* qitem= NULL;
  assert(qitemStore->first != NULL);
  assert(qitemStore->last != NULL);
  if (qitemStore->first != qitemStore->last) {
    qitem= qitemStore->first;
    assert(qitem->next != NULL);
    qitemStore->first= qitem->next;
  }
  return qitem;
}







//Sólo se debe usar en main/node's thread !
static eventsQueue* qitemStoreInit (void) {
  TAGG_DEBUG && printf("Q_ITEM_STORE_INIT\n");
  eventsQueue* queue= (eventsQueue*) calloc(1, sizeof(eventsQueue));
  eventsQueueItem* qitem= queue->first= (eventsQueueItem*) calloc(1, sizeof(eventsQueueItem));
  int i= 2048;
  while (i--) {
    qitem->next= (eventsQueueItem*) calloc(1, sizeof(eventsQueueItem));
    qitem= qitem->next;
  }
  queue->last= qitem;
  return queue;
}







//Sólo se debe usar en main/node's thread !
static void destroyQueue (eventsQueue* queue) {
  TAGG_DEBUG && printf("Q_DESTROY_QUEUE\n");
  eventsQueueItem* qitem;
  assert(queue->first != NULL);
  while (queue->first) {
    qitem= queue->first;
    queue->first= qitem->next;
    qitemStorePush(qitem);
  }
  free(queue);
}







//Llamar a un método de la thread con el 'this' (receiver) mal puesto es bombazo seguro, por eso esto.
static typeThread* isAThread (v8::Handle<v8::Object> receiver) {
  typeThread* thread;
  if (receiver->IsObject()) {
    if (receiver->InternalFieldCount() == 1) {
      thread= (typeThread*) receiver->GetPointerFromInternalField(0);
      assert(thread != NULL);
      if (thread && (thread->threadMagicCookie == kThreadMagicCookie)) {
        return thread;
      }
    }
  }
  return NULL;
}







//Se encarga de poner en marcha la thread si es que estaba durmiendo
static void wakeUpThread (typeThread* thread, int sigkill) {

//Esto se ejecuta siempre en node's main thread

  TAGG_DEBUG && printf("THREAD %ld wakeUpThread(sigkill=%d) #1\n", thread->id, sigkill);
  
  //Cogiendo este lock sabemos que la thread o no ha salido aún
  //del event loop o está parada en wait/sleep/idle
  pthread_mutex_lock(&thread->idle_mutex);
  
  TAGG_DEBUG && printf("THREAD %ld wakeUpThread(sigkill=%d) #2\n", thread->id, sigkill);
  //Estamos seguros de que no se está tocando thread->IDLE
  //xq tenemos el lock nosotros y sólo se toca con el lock puesto
  
  //Es un error volver llamar a esto después de un sigkill
  assert(!thread->sigkill);
  thread->sigkill= sigkill;
  if (thread->IDLE) {
    //estaba parada, hay que ponerla en marcha
    TAGG_DEBUG && printf("THREAD %ld wakeUpThread(sigkill=%d) #3\n", thread->id, sigkill);
    pthread_cond_signal(&thread->idle_cv);
  }
  //Hay que volver a soltar el lock
  pthread_mutex_unlock(&thread->idle_mutex);
  TAGG_DEBUG && printf("THREAD %ld wakeUpThread(sigkill=%d) #5 EXIT\n", thread->id, sigkill);
/*
  if (thread->sigkill == kKillRudely) {
    thread->isolate->TerminateExecution();
    printf("THREAD %ld wakeUpThread(sigkill=%d) TerminateExecution() #6 EXIT\n", thread->id, sigkill);
  }
*/
}







//printf de andar por casa
static v8::Handle<v8::Value> Puts (const v8::Arguments &args) {
  int i= 0;
  while (i < args.Length()) {
    v8::HandleScope scope;
    v8::String::Utf8Value c_str(args[i]);
    fputs(*c_str, stdout);
    i++;
  }
  fflush(stdout);
  return v8::Undefined();
}








//Esto es lo primero que se ejecuta en la(s) thread(s) al nacer.
//Básicamente inicializa lo necesario y se entra en el eventloop
static void* threadBootProc (void* arg) {

  int dummy;
  pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &dummy);
  pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, &dummy);
  
  typeThread* thread= (typeThread*) arg;
  
  TAGG_DEBUG && printf("THREAD %ld BOOT ENTER\n", thread->id);
  
  thread->isolate= v8::Isolate::New();
  thread->isolate->SetData(thread);
  
  if (useLocker) {
    //TAGG_DEBUG && printf("**** USING LOCKER: YES\n");
    v8::Locker myLocker(thread->isolate);
    //v8::Isolate::Scope isolate_scope(thread->isolate);
    eventLoop(thread);
  }
  else {
    eventLoop(thread);
  }
  
  TAGG_DEBUG && printf("THREAD %ld BOOT EXIT #1\n", thread->id);
  thread->isolate->Exit();
  TAGG_DEBUG && printf("THREAD %ld BOOT EXIT #2\n", thread->id);
  thread->isolate->Dispose();
  TAGG_DEBUG && printf("THREAD %ld BOOT EXIT #3\n", thread->id);
  thread->ended= 1;
  TAGG_DEBUG && printf("THREAD %ld BOOT EXIT #4 WAKEUP_NODE_EVENT_LOOP\n", thread->id);
  WAKEUP_NODE_EVENT_LOOP
  TAGG_DEBUG && printf("THREAD %ld BOOT EXIT #5 ENDED\n", thread->id);
  return 0;
}








static inline char* o2cstr (v8::Handle<v8::Value> o) {
  v8::String::Utf8Value utf8(o);
  long len= utf8.length();
  char* r= (char*) malloc( (len + 1) * sizeof(char));
  memcpy(r, *utf8, len);
  r[len]= 0;
  return r;
}








// The thread's eventloop runs in the thread(s) not in node's main thread
static void eventLoop (typeThread* thread) {
  TAGG_DEBUG && printf("THREAD %ld EVENTLOOP ENTER\n", thread->id);

  thread->isolate->Enter();
  v8::Persistent<v8::Context> context= v8::Context::New();
  context->Enter();
  {
  v8::HandleScope scope1;
  
  v8::Local<v8::Object> global= context->Global();
  global->Set(v8::String::NewSymbol("puts"), v8::FunctionTemplate::New(Puts)->GetFunction());
  v8::Local<v8::Object> threadObject= v8::Object::New();
  threadObject->Set(v8::String::NewSymbol("id"), v8::Number::New(thread->id));
  threadObject->Set(v8::String::NewSymbol("version"),v8::String::New(k_TAGG_VERSION));
  threadObject->Set(v8::String::NewSymbol("emit"), v8::FunctionTemplate::New(threadEmit)->GetFunction());
  v8::Local<v8::Object> script= v8::Local<v8::Object>::New(v8::Script::Compile(v8::String::New(kBoot_js))->Run()->ToObject());
  v8::Local<v8::Object> r= script->CallAsFunction(threadObject, 0, NULL)->ToObject();
  v8::Local<v8::Object> dnt= r->Get(v8::String::NewSymbol("dnt"))->ToObject();
  v8::Local<v8::Object> dev= r->Get(v8::String::NewSymbol("dev"))->ToObject();
  
  //SetFatalErrorHandler(FatalErrorCB);
    
  while (1) {
  
      double ntql;
      eventsQueueItem* qitem= NULL;
      eventsQueueItem* event;
      eventsQueueItem* qitem3;
      v8::TryCatch onError;
        
      TAGG_DEBUG && printf("THREAD %ld BEFORE WHILE\n", thread->id);

      while (1) {
          
          TAGG_DEBUG && printf("THREAD %ld WHILE\n", thread->id);
          
          if (thread->sigkill == kKillRudely) break;
          else if (qitem || (qitem= qPull(thread->threadEventsQueue))) {
          
            event= qitem;
            qitem= NULL;
            TAGG_DEBUG && printf("THREAD %ld QITEM\n", thread->id);
            if (event->eventType == eventTypeLoad) {
              v8::HandleScope scope;
              
              v8::Local<v8::Script> script;
              v8::Local<v8::Value> resultado;
              
              TAGG_DEBUG && printf("THREAD %ld QITEM LOAD\n", thread->id);
              
              char* buf= NULL;
              assert(event->load.path != NULL);
              FILE* fp= fopen(event->load.path, "rb");
              free(event->load.path);
              
              if (fp) {
                fseek(fp, 0, SEEK_END);
                long len= ftell(fp);
                rewind(fp); //fseek(fp, 0, SEEK_SET);
                buf= (char*) calloc(len + 1, sizeof(char)); // +1 to get null terminated string
                fread(buf, len, 1, fp);
                fclose(fp);
              }
              
              if (buf != NULL) {
                script= v8::Script::Compile(v8::String::New(buf));
                free(buf);
                if (!onError.HasCaught()) resultado= script->Run();
                event->load.error= onError.HasCaught() ? 1 : 0;
              }
              else {
                event->load.error= 2;
              }
              
              if (event->load.hasCallback) {
                qitem3= nuQitem(thread->processEventsQueue);
                qitem3->eval.error= event->load.error;
                if (!qitem3->eval.error)
                  qitem3->eval.resultado= o2cstr(resultado);
                else if (qitem3->eval.error == 1)
                  qitem3->eval.resultado= o2cstr(onError.Exception());
                else
                  qitem3->eval.resultado= strdup("fopen(path) error");
                qitem3->callback= event->callback;
                qitem3->load.hasCallback= 1;
                qitem3->eventType= eventTypeEval;
                qPush(qitem3, thread->processEventsQueue);
                WAKEUP_NODE_EVENT_LOOP
              }
              
              if (onError.HasCaught()) onError.Reset();
              
              event->eventType= eventTypeNone;
            }
            else if (event->eventType == eventTypeEval) {
              v8::HandleScope scope;
              
              v8::Local<v8::Script> script;
              v8::Local<v8::Value> resultado;
              
              TAGG_DEBUG && printf("THREAD %ld QITEM EVAL\n", thread->id);
              
              script= v8::Script::New(v8::String::New(event->eval.scriptText));
              free(event->eval.scriptText);
            
              if (!onError.HasCaught())
                resultado= script->Run();
              event->eval.error= onError.HasCaught() ? 1 : 0;

              if (event->eval.hasCallback) {
                qitem3= nuQitem(thread->processEventsQueue);
                qitem3->eval.error= event->eval.error;
                if (!qitem3->eval.error)
                  qitem3->eval.resultado= o2cstr(resultado);
                else
                  qitem3->eval.resultado= o2cstr(onError.Exception());
                qitem3->callback= event->callback;
                qitem3->eval.hasCallback= 1;
                qitem3->eventType= eventTypeEval;
                qPush(qitem3, thread->processEventsQueue);
                WAKEUP_NODE_EVENT_LOOP
              }
              
              if (onError.HasCaught()) onError.Reset();
              
              event->eventType= eventTypeNone;
            }
            else if (event->eventType == eventTypeEmit) {
              v8::HandleScope scope;
              
              v8::Local<v8::Array> array;
              v8::Local<v8::Value> args[2];
              
              TAGG_DEBUG && printf("THREAD %ld QITEM EVENT #%ld\n", thread->id, event->serial);
              
              assert(event->emit.eventName != NULL);
              args[0]= v8::String::New(event->emit.eventName);
              args[1]= array= v8::Array::New(event->emit.argc);
              if (event->emit.argc) {
                int i= 0;
                while (i < event->emit.argc) {
                  array->Set(i, v8::String::New(event->emit.argv[i]));
                  free(event->emit.argv[i]);
                  i++;
                }
                free(event->emit.argv);
              }
              
              dev->CallAsFunction(global, 2, args);
              free(event->emit.eventName);
              event->eventType= eventTypeNone;
            }
            else {
              assert(0);
            }
          }
          else
            TAGG_DEBUG && printf("THREAD %ld NO QITEM\n", thread->id);

          if (thread->sigkill == kKillRudely) break;
          else {
            v8::HandleScope scope;
            TAGG_DEBUG && printf("THREAD %ld NTQL\n", thread->id);
            ntql= dnt->CallAsFunction(global, 0, NULL)->ToNumber()->Value();
            if (onError.HasCaught()) onError.Reset();
          }
          
          if (thread->sigkill == kKillRudely) break;
          else if (!ntql && !(qitem || (qitem= qPull(thread->threadEventsQueue)))) {
            TAGG_DEBUG && printf("THREAD %ld EXIT WHILE: NO NTQL AND NO QITEM\n", thread->id);
            break;
          }
          
        }

      if (thread->sigkill) break;

      v8::V8::IdleNotification();
      
      TAGG_DEBUG && printf("THREAD %ld BEFORE MUTEX\n", thread->id);
      //cogemos el lock para
      //por un lado poder mirar si hay cosas en la queue sabiendo
      //que nadie la está tocando
      //y por otro lado para poder tocar thread->IDLE sabiendo
      //que nadie la está mirando mientras la tocamos.
      pthread_mutex_lock(&thread->idle_mutex);
      TAGG_DEBUG && printf("THREAD %ld TIENE threadEventsQueue_MUTEX\n", thread->id);
      //aquí tenemos acceso exclusivo a threadEventsQueue y a thread->IDLE
      while (!(qitem || (qitem= qPull(thread->threadEventsQueue))) && !thread->sigkill) {
        //sólo se entra aquí si no hay nada en la queue y no hay sigkill
        //hemos avisado con thread->IDLE de que nos quedamos parados
        // para que sepan que nos han de despertar
        thread->IDLE= 1;
        if (thread->hasIdleEventsListener) notifyIdle(thread);
        TAGG_DEBUG && printf("THREAD %ld SLEEP\n", thread->id);
        //en pthread_cond_wait se quedará atascada esta thread hasta que
        //nos despierten y haya cosas en la queue o haya sigkill
        //El lock se abre al entrar en pthread_cond_wait así que los
        //demás ahora van a poder mirar thread->IDLE mientras estamos parados/durmiendo
        pthread_cond_wait(&thread->idle_cv, &thread->idle_mutex);
        //El lock queda cerrado al salir de pthread_cond_wait pero no importa xq
        //si seguimos en el bucle se va a volver a abrir y si salimos tb
      }
      //Aquí aún tenemos el lock así que podemos tocar thread->IDLE con seguridad
      thread->IDLE= 0;
      TAGG_DEBUG && printf("THREAD %ld WAKE UP\n", thread->id);
      //lo soltamos
      pthread_mutex_unlock(&thread->idle_mutex);
      TAGG_DEBUG && printf("THREAD %ld SUELTA threadEventsQueue_mutex\n", thread->id);
      
    }
  
  }
  context->Exit();
  context.Dispose();
  TAGG_DEBUG && printf("THREAD %ld EVENTLOOP EXIT\n", thread->id);
}







//Cuando una thread se echa a dormir esto lo debe notificar a node. OJO TODO
static void notifyIdle (typeThread* thread) {
  printf("*** notifyIdle()\n");
}








//Esto es por culpa de libuv que se empeña en tener un callback de terminación. Al parecer...
static void cleanUpAfterThreadCallback (uv_handle_t* arg) {
  v8::HandleScope scope;
  typeThread* thread= (typeThread*) arg;
  TAGG_DEBUG && printf("THREAD %ld cleanUpAfterThreadCallback()\n", thread->id);
  if (thread->hasDestroyCallback) {
    thread->destroyCallback->CallAsFunction(v8::Context::GetCurrent()->Global(), 0, NULL);
  }
  thread->destroyCallback.Dispose();
  free(thread);
}








//Deshacerse de todo, lo que se pueda guardar se guarda para reutilizarlo
static void cleanUpAfterThread (typeThread* thread) {
  
  TAGG_DEBUG && printf("THREAD %ld cleanUpAfterThread() IN MAIN THREAD #1\n", thread->id);
  TAGG_DEBUG && printf("THREAD %ld cleanUpAfterThread() destroyQueue(thread->threadEventsQueue)\n", thread->id);
  destroyQueue(thread->threadEventsQueue);
  TAGG_DEBUG && printf("THREAD %ld cleanUpAfterThread() destroyQueue(thread->processEventsQueue)\n", thread->id);
  destroyQueue(thread->processEventsQueue);
  
  pthread_cond_destroy(&(thread->idle_cv));
  pthread_mutex_destroy(&(thread->idle_mutex));
  thread->nodeDispatchEvents.Dispose();
  thread->nodeObject.Dispose();  //OJO Y SI QUEDAN OTRAS REFERENCIAS POR AHÍ QUÉ PASA?
  
  if (thread->ended) {
    // Esta thread llegó a funcionar alguna vez
    // hay que apagar uv antes de poder hacer free(thread)
    // De hecho el free(thread) se hará en una Callabck xq uv_close la va a llamar
    
    TAGG_DEBUG && printf("THREAD %ld cleanUpAfterThread() FREE IN UV CALLBACK #2\n", thread->id);
    
#ifdef TAGG_USE_LIBUV
    uv_close((uv_handle_t*) &thread->async_watcher, cleanUpAfterThreadCallback);
    //uv_unref(&thread->async_watcher);
#else
    ev_async_stop(EV_DEFAULT_UC_ &thread->async_watcher);
    ev_unref(EV_DEFAULT_UC);
    cleanUpAfterThreadCallback((uv_handle_t*) thread);
#endif

  }
  else {
    //Esta thread nunca ha llegado a arrancar
    //Seguramente venimos de un error en thread.create())
    TAGG_DEBUG && printf("THREAD %ld cleanUpAfterThread() FREE HERE #3\n", thread->id);
    free(thread);
  }
}








// C callback that runs in node's main thread. This is called by node's event loop
// when the thread tells it to do so. This is the one responsible for
// calling the thread's JS callback in node's js context in node's main thread.
static void Callback (
#ifdef TAGG_USE_LIBUV
  uv_async_t* watcher
#else
  EV_P_ ev_async* watcher
#endif
                           , int status) {
                           
  v8::HandleScope scope;
  
  eventsQueueItem* event;
  typeThread* thread= (typeThread*) watcher;
  
  v8::Local<v8::Array> array;
  v8::Local<v8::Value> args[2];
  v8::Local<v8::Value> null= v8::Local<v8::Value>::New(v8::Null());
  
  assert(thread != NULL);
  assert(!thread->destroyed);
  
  v8::TryCatch onError;
  while ((event= qPull(thread->processEventsQueue))) {
  
    TAGG_DEBUG && printf("CALLBACK %ld IN MAIN THREAD\n", thread->id);

    assert(event != NULL);

    if (event->eventType == eventTypeEval) {
    
      TAGG_DEBUG && printf("CALLBACK eventTypeEval IN MAIN THREAD\n");
      
      assert(event->eval.hasCallback);
      assert(event->eval.resultado != NULL);
      
      if (event->eval.error) {
        args[0]= v8::Exception::Error(v8::String::New(event->eval.resultado));
        args[1]= null;
      }
      else {
        args[0]= null;
        args[1]= v8::String::New(event->eval.resultado);
      }
      event->callback->CallAsFunction(thread->nodeObject, 2, args);
      
      event->callback.Dispose();
      free(event->eval.resultado);
      event->eventType = eventTypeNone;
      
      if (onError.HasCaught()) {
        node::FatalException(onError);
        return;
      }
    }
    else if (event->eventType == eventTypeEmit) {
    
      TAGG_DEBUG && printf("CALLBACK eventTypeEmit IN MAIN THREAD\n");
      
      args[0]= v8::String::New(event->emit.eventName);
      array= v8::Array::New(event->emit.argc);
      args[1]= array;
      
      if (event->emit.argc) {
        int i= 0;
        while (i < event->emit.argc) {
          array->Set(i, v8::String::New(event->emit.argv[i]));
          free(event->emit.argv[i]);
          i++;
        }
        free(event->emit.argv);
      }

      thread->nodeDispatchEvents->CallAsFunction(v8::Context::GetCurrent()->Global(), 2, args);
      
      free(event->emit.eventName);
      event->eventType = eventTypeNone;
    }
    else {
      assert(0);
    }
    
    event->eventType = eventTypeNone;
    event= NULL;
  }
  
  if (thread->sigkill && thread->ended) {
    TAGG_DEBUG && printf("THREAD %ld CALLBACK CALLED cleanUpAfterThread()\n", thread->id);
    //pthread_cancel(thread->thread);
    thread->destroyed= 1;
    cleanUpAfterThread(thread);
  }
}








// Tell a thread to quit, either nicely or rudely.
static v8::Handle<v8::Value> Destroy (const v8::Arguments &args) {

  //thread.destroy() or thread.destroy(0) means nicely (the default)
  //thread destroy(1) means rudely.
  //When done nicely the thread will quit only if/when there aren't anymore jobs pending
  //in its jobsQueue nor nextTick()ed functions to execute in the nextTick queue _ntq[]
  //When done rudely it will try to exit the event loop regardless.
  //ToDo: If the thread is stuck in a ` while (1) ; ` or something this won't work...
  
  v8::HandleScope scope;
  //TODO: Hay que comprobar que this en un objeto y que tiene hiddenRefTotypeThread_symbol y que no es nil
  //TODO: Aquí habría que usar static void TerminateExecution(int thread_id);
  //TODO: static void v8::V8::TerminateExecution  ( Isolate *   isolate= NULL   )
  
  typeThread* thread= isAThread(args.This());
  if (!thread) {
    return v8::ThrowException(v8::Exception::TypeError(v8::String::New("thread.destroy(): the receiver must be a thread object")));
  }
  
  int nuSigkill= kKillNicely;
  if (args.Length()) {
    nuSigkill= args[0]->ToNumber()->Value() ? kKillRudely : kKillNicely;
  }
  
  thread->hasDestroyCallback= (args.Length() > 1) && (args[1]->IsFunction());
  if (thread->hasDestroyCallback) {
    thread->destroyCallback= v8::Persistent<v8::Object>::New(args[1]->ToObject());
  }
  
  if (TAGG_DEBUG) {
    const char* str= (nuSigkill == kKillNicely ? "NICELY" : "RUDELY");
    printf("THREAD %ld DESTROY(%s) #1\n", thread->id, str);
  }
  
  wakeUpThread(thread, nuSigkill);
  return v8::Undefined();
}








// Eval: Pushes an eval job to the threadEventsQueue.
static v8::Handle<v8::Value> Eval (const v8::Arguments &args) {
  v8::HandleScope scope;
  
  if (!args.Length()) {
    return v8::ThrowException(v8::Exception::TypeError(v8::String::New("thread.eval(program [,callback]): missing arguments")));
  }
  
  typeThread* thread= isAThread(args.This());
  if (!thread) {
    return v8::ThrowException(v8::Exception::TypeError(v8::String::New("thread.eval(): the receiver must be a thread object")));
  }

  eventsQueueItem* event= nuQitem(thread->threadEventsQueue);
  event->eval.hasCallback= (args.Length() > 1) && (args[1]->IsFunction());
  if (event->eval.hasCallback) {
    event->callback= v8::Persistent<v8::Object>::New(args[1]->ToObject());
  }
  event->eval.scriptText= o2cstr(args[0]);
  event->eventType= eventTypeEval;
  qPush(event, thread->threadEventsQueue);
  wakeUpThread(thread, thread->sigkill);
  return args.This();
}








// Load: emits a eventTypeLoad event to the thread
static v8::Handle<v8::Value> Load (const v8::Arguments &args) {
  v8::HandleScope scope;

  if (!args.Length()) {
    return v8::ThrowException(v8::Exception::TypeError(v8::String::New("thread.load(filename [,callback]): missing arguments")));
  }

  typeThread* thread= isAThread(args.This());
  if (!thread) {
    return v8::ThrowException(v8::Exception::TypeError(v8::String::New("thread.load(): the receiver must be a thread object")));
  }
  
  eventsQueueItem* event= nuQitem(thread->threadEventsQueue);
  event->eventType= eventTypeLoad;
  event->load.path= o2cstr(args[0]);
  event->load.hasCallback= ((args.Length() > 1) && (args[1]->IsFunction()));
  if (event->load.hasCallback) {
    event->callback= v8::Persistent<v8::Object>::New(args[1]->ToObject());
  }
  qPush(event, thread->threadEventsQueue);
  wakeUpThread(thread, thread->sigkill);
  return args.This();
}







//No se usa xq parece que el inline no va, pero sirve para acortar processEmit y threadEmit,
//por que casi todo el código es idéntico en ambas
static inline void pushEmitEvent (eventsQueue* queue, const v8::Arguments &args) {

  eventsQueueItem* event= nuQitem(queue);
  event->emit.eventName= o2cstr(args[0]);
  event->emit.argc= (args.Length() > 1) ? (args.Length() - 1) : 0;
  if (event->emit.argc) {
    event->emit.argv= (char**) malloc(event->emit.argc * sizeof(char*));
    int i= 0;
    while (i < event->emit.argc) {
      event->emit.argv[i]= o2cstr(args[i+1]);
      i++;
    }
  }
  
  TAGG_DEBUG && printf("PROCESS EMIT TO THREAD #%ld\n", event->serial);
  
  event->eventType= eventTypeEmit;
  qPush(event, queue);
  
}







//La que emite los events de node hacia las threads
static v8::Handle<v8::Value> processEmit (const v8::Arguments &args) {
  if (!args.Length()) return args.This();
  typeThread* thread= isAThread(args.This());
  if (!thread) {
    return v8::ThrowException(v8::Exception::TypeError(v8::String::New("thread.emit(): 'this' must be a thread object")));
  }
/*
  eventsQueueItem* event= nuQitem(thread->threadEventsQueue);
  event->serial= serial++;
  event->emit.eventName= o2cstr(args[0]);
  event->emit.argc= (args.Length() > 1) ? (args.Length() - 1) : 0;
  if (event->emit.argc) {
    event->emit.argv= (char**) malloc(event->emit.argc * sizeof(char*));
    int i= 0;
    while (i < event->emit.argc) {
      event->emit.argv[i]= o2cstr(args[i+1]);
      i++;
    }
  }
  
  TAGG_DEBUG && printf("PROCESS EMIT TO THREAD %ld #%ld\n", thread->id, event->serial);
  
  event->eventType= eventTypeEmit;
  qPush(event, thread->threadEventsQueue);
*/
  pushEmitEvent(thread->threadEventsQueue, args);
  wakeUpThread(thread, thread->sigkill);
  return args.This();
}







//La que emite los events de las threads hacia node
static v8::Handle<v8::Value> threadEmit (const v8::Arguments &args) {
  if (!args.Length()) return args.This();
  typeThread* thread= (typeThread*) v8::Isolate::GetCurrent()->GetData();
  assert(thread != NULL);
  assert(thread->threadMagicCookie == kThreadMagicCookie);
/*
  eventsQueueItem* event= nuQitem(thread->processEventsQueue);
  event->serial= serial++;
  event->emit.eventName= o2cstr(args[0]);
  event->emit.argc= (args.Length() > 1) ? (args.Length() - 1) : 0;
  if (event->emit.argc) {
    event->emit.argv= (char**) malloc(event->emit.argc * sizeof(char*));
    int i= 0;
    while (i < event->emit.argc) {
      event->emit.argv[i]= o2cstr(args[i+1]);
      i++;
    }
  }
  
  TAGG_DEBUG && printf("THREAD %ld EMIT #%ld\n", thread->id, event->serial);
  
  event->eventType= eventTypeEmit;
  qPush(event, thread->processEventsQueue);
*/
  pushEmitEvent(thread->processEventsQueue, args);
  WAKEUP_NODE_EVENT_LOOP
  return args.This();
}








//Se ejecuta al hacer tagg.create(): Creates and launches a new isolate in a new background thread.
static v8::Handle<v8::Value> Create (const v8::Arguments &args) {
    v8::HandleScope scope;
    
    typeThread* thread= (typeThread*) calloc(1, sizeof (typeThread));
    thread->id= threadsCtr++;
    thread->threadMagicCookie= kThreadMagicCookie;
    thread->threadEventsQueue= nuQueue();
    thread->processEventsQueue= nuQueue();
    thread->nodeObject= v8::Persistent<v8::Object>::New(threadTemplate->NewInstance());
    thread->nodeObject->SetPointerInInternalField(0, thread);
    thread->nodeObject->Set(id_symbol, v8::Integer::New(thread->id));
    thread->nodeObject->Set(version_symbol, v8::String::New(k_TAGG_VERSION));
    thread->nodeDispatchEvents= v8::Persistent<v8::Object>::New(boot_js->CallAsFunction(thread->nodeObject, 0, NULL)->ToObject());
    
    pthread_cond_init(&(thread->idle_cv), NULL);
    pthread_mutex_init(&(thread->idle_mutex), NULL);
    
    char* errstr;
    int err, retry= 5;
    do {
      err= pthread_create(&(thread->thread), NULL, threadBootProc, thread);
      //pthread_detach(pthread_t thread); ???
      if (!err) break;
      errstr= strerror(err);
      printf("THREAD %ld PTHREAD_CREATE() ERROR %d : %s RETRYING %d\n", thread->id, err, errstr, retry);
      usleep(100000);
    } while (--retry);
    
    if (err) {
      //Algo ha ido mal, toca deshacer todo
      printf("THREAD %ld PTHREAD_CREATE() ERROR %d : %s NOT RETRYING ANY MORE\n", thread->id, err, errstr);
      TAGG_DEBUG && printf("CALLED cleanUpAfterThread %ld FROM CREATE()\n", thread->id);
      cleanUpAfterThread(thread);
      return v8::ThrowException(v8::Exception::TypeError(v8::String::New("create(): error in pthread_create()")));
    }
    else {
    
#ifdef TAGG_USE_LIBUV
      uv_async_init(uv_default_loop(), &thread->async_watcher, Callback);
#else
      ev_async_init(&thread->async_watcher, Callback);
      ev_async_start(EV_DEFAULT_UC_ &thread->async_watcher);
      ev_ref(EV_DEFAULT_UC);
#endif
    
    }

    return thread->nodeObject;
}







//Esto es lo primero que llama node al hacer require('threads_a_gogo')
void Init (v8::Handle<v8::Object> target) {
  qitemStore= qitemStoreInit();
  useLocker= v8::Locker::IsActive();
  id_symbol= v8::Persistent<v8::String>::New(v8::String::NewSymbol("id"));
  version_symbol= v8::Persistent<v8::String>::New(v8::String::NewSymbol("version"));
  boot_js= v8::Persistent<v8::Object>::New(v8::Script::Compile(v8::String::New(kBoot_js))->Run()->ToObject());
  
  threadTemplate= v8::Persistent<v8::ObjectTemplate>::New(v8::ObjectTemplate::New());
  threadTemplate->SetInternalFieldCount(1);
  threadTemplate->Set(v8::String::NewSymbol("load"), v8::FunctionTemplate::New(Load));
  threadTemplate->Set(v8::String::NewSymbol("eval"), v8::FunctionTemplate::New(Eval));
  threadTemplate->Set(v8::String::NewSymbol("emit"), v8::FunctionTemplate::New(processEmit));
  threadTemplate->Set(v8::String::NewSymbol("destroy"), v8::FunctionTemplate::New(Destroy));
  
  target->Set(v8::String::NewSymbol("create"), v8::FunctionTemplate::New(Create)->GetFunction());
  target->Set(v8::String::NewSymbol("createPool"), v8::Script::Compile(v8::String::New(kPool_js))->Run()->ToObject());
  target->Set(version_symbol, v8::String::New(k_TAGG_VERSION));
}

NODE_MODULE(threads_a_gogo, Init)

/*
gcc -E -I /Users/jorge/JAVASCRIPT/binarios/include/node -o /o.c /Users/jorge/JAVASCRIPT/threads_a_gogo/src/threads_a_gogo.cc && mate /o.c

tagg=require('threads_a_gogo')
process.versions
t=tagg.create()
*/