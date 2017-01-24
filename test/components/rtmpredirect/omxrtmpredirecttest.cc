#include <xlog.h>
#include <xmacro.h>
#include <webrtc/base/thread.h>
#include <webrtc/base/physicalsocketserver.h>
#include <signal.h>
#include <pthread.h>
#include <user_debug_levels.h>

#include "omxrtmpredirecttest.h"

class CustomSocketServer : public rtc::PhysicalSocketServer {
public:
  CustomSocketServer(rtc::Thread *thread, appPrivateType *app_priv)
    : thread_(thread), app_priv_(app_priv) { }
  virtual ~CustomSocketServer() { }

  virtual bool Wait(int cms, bool process_io) override {
    if (app_priv_->bEOS) {
      thread_->Quit();
    }
    return rtc::PhysicalSocketServer::Wait(10, process_io);
  }

protected:
  rtc::Thread *thread_;
  appPrivateType *app_priv_;
};

class SignalProcessThread : public rtc::Thread {
public:
  SignalProcessThread(sigset_t *set, Thread *main_thread, appPrivateType *app_priv)
    : set_(set), main_thread_(main_thread), app_priv_(app_priv) { }
  virtual ~SignalProcessThread() { }
  virtual void Run();

private:
  sigset_t *set_;
  Thread *main_thread_;
  appPrivateType *app_priv_;
};

class FunctorQuit {
public:
  explicit FunctorQuit(appPrivateType *app_priv)
    : app_priv_(app_priv) { }
  void operator()() {
    app_priv_->bEOS = OMX_TRUE;
  }

private:
  appPrivateType *app_priv_;
};

void SignalProcessThread::Run()
{
  int ret, sig;

  for ( ; ; ) {
    ret = sigwait(set_, &sig);
    if (ret != 0) {
      LOGE("sigwait failed: %s", ERRNOMSG);
      break;
    }

    if (sig == SIGINT) {
      LOGI("Program received signal %d", sig);
      FunctorQuit f(app_priv_);
      main_thread_->Invoke<void>(f);
      break;
    }
  }
}

static void setHeader(OMX_PTR header, OMX_U32 size)
{
  OMX_VERSIONTYPE* ver = (OMX_VERSIONTYPE*) ((OMX_U8 *) header + sizeof(OMX_U32));
  *((OMX_U32*) header) = size;

  ver->s.nVersionMajor = VERSIONMAJOR;
  ver->s.nVersionMinor = VERSIONMINOR;
  ver->s.nRevision = VERSIONREVISION;
  ver->s.nStep = VERSIONSTEP;
}

static OMX_CALLBACKTYPE rtmpsrccallbacks = {
  .EventHandler     = rtmpsrc_event_handler,
  .EmptyBufferDone  = NULL,
  .FillBufferDone   = rtmpsrc_fillbuffer_done
};
static OMX_CALLBACKTYPE rtmpoutcallbacks = {
  .EventHandler     = rtmpout_event_handler,
  .EmptyBufferDone  = rtmpout_emptybuffer_done,
  .FillBufferDone   = NULL
};
static OMX_CALLBACKTYPE clocksrccallbacks = {
  .EventHandler     = clocksrc_event_handler,
  .EmptyBufferDone  = NULL,
  .FillBufferDone   = clocksrc_fillbuffer_done
};
static OMX_CALLBACKTYPE videoschdcallbacks = {
  .EventHandler     = videoschd_event_handler,
  .EmptyBufferDone  = videoschd_emptybuffer_done,
  .FillBufferDone   = videoschd_fillbuffer_done
};

void set_header(OMX_PTR header, OMX_U32 size)
{
  OMX_VERSIONTYPE* ver = (OMX_VERSIONTYPE*) ((OMX_U8 *) header + sizeof(OMX_U32));
  *((OMX_U32*) header) = size;

  ver->s.nVersionMajor = VERSIONMAJOR;
  ver->s.nVersionMinor = VERSIONMINOR;
  ver->s.nRevision = VERSIONREVISION;
  ver->s.nStep = VERSIONSTEP;
}

int main(int argc, const char *argv[])
{
  appPrivateType *app_priv;
  OMX_ERRORTYPE omx_err;

  xlog::log_add_dst("./omxrtmpredirecttest.log");

  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  pthread_sigmask(SIG_BLOCK, &set, NULL);

  app_priv = (appPrivateType *) calloc(1, sizeof(appPrivateType));
  app_priv->rtmpsrc_event_sem = (tsem_t *) malloc(sizeof(tsem_t));
  app_priv->rtmpout_event_sem = (tsem_t *) malloc(sizeof(tsem_t));
  app_priv->clocksrc_event_sem = (tsem_t *) malloc(sizeof(tsem_t));
  app_priv->videoschd_event_sem = (tsem_t *) malloc(sizeof(tsem_t));
  app_priv->bEOS = OMX_FALSE;

  tsem_init(app_priv->rtmpsrc_event_sem, 0);
  tsem_init(app_priv->rtmpout_event_sem, 0);
  tsem_init(app_priv->clocksrc_event_sem, 0);
  tsem_init(app_priv->videoschd_event_sem, 0);

  rtc::AutoThread auto_thread;
  rtc::Thread *thread = rtc::Thread::Current();
  CustomSocketServer socket_server(thread, app_priv);
  thread->set_socketserver(&socket_server);

  SignalProcessThread spt(&set, thread, app_priv);
  spt.Start();

  omx_err = OMX_Init();

  omx_err = OMX_GetHandle(&app_priv->rtmpsrchandle, (OMX_STRING) "OMX.st.rtmpsrc", app_priv, &rtmpsrccallbacks);
  omx_err = OMX_GetHandle(&app_priv->rtmpouthandle, (OMX_STRING) "OMX.st.rtmpout", app_priv, &rtmpoutcallbacks);
  omx_err = OMX_GetHandle(&app_priv->clocksrchandle, (OMX_STRING) "OMX.st.clocksrc", app_priv, &clocksrccallbacks);
  omx_err = OMX_GetHandle(&app_priv->videoschdhandle, (OMX_STRING) "OMX.st.video.scheduler", app_priv, &videoschdcallbacks);

  OMX_INDEXTYPE omx_index;
  char url[1024];
  omx_err = OMX_GetExtensionIndex(app_priv->rtmpsrchandle, (OMX_STRING) "OMX.ST.index.param.inputurl",
                                  &omx_index);
  omx_err = OMX_SetParameter(app_priv->rtmpsrchandle, omx_index, (OMX_PTR) "rtmp://127.0.0.1/live/va");
  OMX_GetParameter(app_priv->rtmpsrchandle, omx_index, url);
  LOGI("Test rtmpsrc url set to: %s\"", url);

  omx_err = OMX_GetExtensionIndex(app_priv->rtmpouthandle, (OMX_STRING) "OMX.ST.index.param.outputurl",
                                  &omx_index);
  omx_err = OMX_SetParameter(app_priv->rtmpouthandle, omx_index, (OMX_PTR) "rtmp://127.0.0.1/live/xyz");
  OMX_GetParameter(app_priv->rtmpouthandle, omx_index, url);
  LOGI("Test rtmpout url set to: %s\"", url);

  omx_err = OMX_SendCommand(app_priv->clocksrchandle, OMX_CommandPortDisable, 1, NULL);
  if (omx_err != OMX_ErrorNone) {
    LOGE("Error in disenable port 1 in clocksrc");
    exit(1);
  }
  omx_err = OMX_SendCommand(app_priv->clocksrchandle, OMX_CommandPortDisable, 2, NULL);
  if (omx_err != OMX_ErrorNone) {
   LOGE("Error in disenable port 2 in clocksrc");
   exit(1);
  }
  tsem_down(app_priv->clocksrc_event_sem);
  tsem_down(app_priv->clocksrc_event_sem);

  omx_err = OMX_SetupTunnel(app_priv->clocksrchandle, 0, app_priv->videoschdhandle, 2);
  if (omx_err != OMX_ErrorNone) {
    LOGE("Setup tunnel btwn clocksrc and video_schd failed");
    exit(1);
  }
  omx_err = OMX_SetupTunnel(app_priv->rtmpsrchandle, 0, app_priv->videoschdhandle, 0);
  if (omx_err != OMX_ErrorNone) {
    LOGE("Setup tunnel btwn rtmpsrc and video_schd failed");
    exit(1);
  }
  omx_err = OMX_SetupTunnel(app_priv->videoschdhandle, 1, app_priv->rtmpouthandle, 0);
  if (omx_err != OMX_ErrorNone) {
    LOGE("Setup tunnel btwn video_schd and rtmpouthandle failed");
    exit(1);
  }
  omx_err = OMX_SetupTunnel(app_priv->rtmpsrchandle, 1, app_priv->rtmpouthandle, 1);
  if (omx_err != OMX_ErrorNone) {
    LOGE("Setup audio tunnel btwn rtmpsrchandle and rtmpouthandle failed");
    exit(1);
  }

  OMX_PARAM_BUFFERSUPPLIERTYPE buffer_supplier;
  buffer_supplier.nPortIndex = 0;
  buffer_supplier.eBufferSupplier = OMX_BufferSupplyInput;
  setHeader(&buffer_supplier, sizeof(OMX_PARAM_BUFFERSUPPLIERTYPE));
  omx_err = OMX_SetParameter(app_priv->videoschdhandle, OMX_IndexParamCompBufferSupplier, &buffer_supplier);
  if (omx_err != OMX_ErrorNone) {
    LOGE("Supplier failed 1");
    exit(1);
  }
  buffer_supplier.nPortIndex = 1;
  buffer_supplier.eBufferSupplier = OMX_BufferSupplyInput;
  setHeader(&buffer_supplier, sizeof(OMX_PARAM_BUFFERSUPPLIERTYPE));
  omx_err = OMX_SetParameter(app_priv->rtmpouthandle, OMX_IndexParamCompBufferSupplier, &buffer_supplier);
  if (omx_err != OMX_ErrorNone) {
    LOGE("Supplier failed 2");
    exit(1);
  }

  omx_err = OMX_SendCommand(app_priv->rtmpsrchandle, OMX_CommandStateSet, OMX_StateIdle, NULL);
  if (omx_err != OMX_ErrorNone) {
   LOGE("Rtmpsrc set to idle state failed");
   exit(1);
  }
  omx_err = OMX_SendCommand(app_priv->rtmpouthandle, OMX_CommandStateSet, OMX_StateIdle, NULL);
  if (omx_err != OMX_ErrorNone) {
    LOGE("Rtmpout set to idle state failed");
    exit(1);
  }
  omx_err = OMX_SendCommand(app_priv->clocksrchandle, OMX_CommandStateSet, OMX_StateIdle, NULL);
  if (omx_err != OMX_ErrorNone) {
    LOGE("Clocksrc set to idle state failed");
    exit(1);
  }
  omx_err = OMX_SendCommand(app_priv->videoschdhandle, OMX_CommandStateSet, OMX_StateIdle, NULL);
  if (omx_err != OMX_ErrorNone) {
    LOGE("Videoschd set to idle state failed");
    exit(1);
  }

  tsem_down(app_priv->rtmpsrc_event_sem);
  LOGI("Rtmpsrc in idle state");
  tsem_down(app_priv->rtmpout_event_sem);
  LOGI("Rtmpout in idle state");
  tsem_down(app_priv->clocksrc_event_sem);
  LOGI("Clocksrc in idle state");
  tsem_down(app_priv->videoschd_event_sem);
  LOGI("Videoschd in idle state");

  omx_err = OMX_SendCommand(app_priv->rtmpsrchandle, OMX_CommandStateSet, OMX_StateExecuting, NULL);
  if (omx_err != OMX_ErrorNone) {
   LOGE("Rtmpsrc set to executing state failed");
   exit(1);
  }
  omx_err = OMX_SendCommand(app_priv->rtmpouthandle, OMX_CommandStateSet, OMX_StateExecuting, NULL);
  if (omx_err != OMX_ErrorNone) {
    LOGE("Rtmpout set to executing state failed");
    exit(1);
  }
  OMX_TIME_CONFIG_ACTIVEREFCLOCKTYPE  sRefClock;
  setHeader(&sRefClock, sizeof(OMX_TIME_CONFIG_ACTIVEREFCLOCKTYPE));
  sRefClock.eClock = OMX_TIME_RefClockAudio;
  OMX_SetConfig(app_priv->clocksrchandle, OMX_IndexConfigTimeActiveRefClock,&sRefClock);

  OMX_TIME_CONFIG_CLOCKSTATETYPE      sClockState;
  OMX_TIME_CONFIG_SCALETYPE           sConfigScale;
  OMX_ERRORTYPE err;
  /* set the clock state to OMX_TIME_ClockStateWaitingForStartTime */
  setHeader(&sClockState, sizeof(OMX_TIME_CONFIG_CLOCKSTATETYPE));
  err = OMX_GetConfig(app_priv->clocksrchandle, OMX_IndexConfigTimeClockState, &sClockState);
  sClockState.nWaitMask = OMX_CLOCKPORT1 || OMX_CLOCKPORT0;  /* wait for audio and video start time */
  sClockState.eState = OMX_TIME_ClockStateWaitingForStartTime;
  err = OMX_SetConfig(app_priv->clocksrchandle, OMX_IndexConfigTimeClockState, &sClockState);
  if(err!=OMX_ErrorNone) {
    LOGE("Error %08x In OMX_SetConfig \n",err);
    exit(1);
  }
  err = OMX_GetConfig(app_priv->clocksrchandle, OMX_IndexConfigTimeClockState, &sClockState);
  omx_err = OMX_SendCommand(app_priv->clocksrchandle, OMX_CommandStateSet, OMX_StateExecuting, NULL);
  if (omx_err != OMX_ErrorNone) {
    LOGE("Clocksrc set to executing state failed");
    exit(1);
  }
  omx_err = OMX_SendCommand(app_priv->videoschdhandle, OMX_CommandStateSet, OMX_StateExecuting, NULL);
  if (omx_err != OMX_ErrorNone) {
    LOGE("Videoschd set to executing state failed");
    exit(1);
  }

  tsem_down(app_priv->rtmpsrc_event_sem);
  LOGI("Rtmpsrc in executing state");
  tsem_down(app_priv->rtmpout_event_sem);
  LOGI("Rtmpout in executing state");
  tsem_down(app_priv->clocksrc_event_sem);
  LOGI("Clocksrc in executing state");
  tsem_down(app_priv->videoschd_event_sem);
  LOGI("Videoschd in executing state");

  LOGI("Running ..");
  thread->Run();

  thread->set_socketserver(NULL);

  omx_err = OMX_SendCommand(app_priv->rtmpsrchandle, OMX_CommandStateSet, OMX_StateIdle, NULL);
  if (omx_err != OMX_ErrorNone) {
   LOGE("Rtmpsrc set to idle state failed");
   exit(1);
  }
  omx_err = OMX_SendCommand(app_priv->rtmpouthandle, OMX_CommandStateSet, OMX_StateIdle, NULL);
  if (omx_err != OMX_ErrorNone) {
    LOGE("Rtmpout set to idle state failed");
    exit(1);
  }
  omx_err = OMX_SendCommand(app_priv->clocksrchandle, OMX_CommandStateSet, OMX_StateIdle, NULL);
  if (omx_err != OMX_ErrorNone) {
    LOGE("Clocksrc set to idle state failed");
    exit(1);
  }
  omx_err = OMX_SendCommand(app_priv->videoschdhandle, OMX_CommandStateSet, OMX_StateIdle, NULL);
  if (omx_err != OMX_ErrorNone) {
    LOGE("Videoschd set to idle state failed");
    exit(1);
  }

  tsem_down(app_priv->rtmpsrc_event_sem);
  LOGI("Rtmpsrc in idle state");
  tsem_down(app_priv->rtmpout_event_sem);
  LOGI("Rtmpout in idle state");
  tsem_down(app_priv->clocksrc_event_sem);
  LOGI("Clocksrc in idle state");
  tsem_down(app_priv->videoschd_event_sem);
  LOGI("Videoschd in idle state");

  omx_err = OMX_SendCommand(app_priv->rtmpsrchandle, OMX_CommandStateSet, OMX_StateLoaded, NULL);
  if (omx_err != OMX_ErrorNone) {
   LOGE("Rtmpsrc set to loaded state failed");
   exit(1);
  }
  omx_err = OMX_SendCommand(app_priv->rtmpouthandle, OMX_CommandStateSet, OMX_StateLoaded, NULL);
  if (omx_err != OMX_ErrorNone) {
    LOGE("Rtmpout set to loaded state failed");
    exit(1);
  }
  omx_err = OMX_SendCommand(app_priv->clocksrchandle, OMX_CommandStateSet, OMX_StateLoaded, NULL);
  if (omx_err != OMX_ErrorNone) {
    LOGE("Clocksrc set to loaded state failed");
    exit(1);
  }
  omx_err = OMX_SendCommand(app_priv->videoschdhandle, OMX_CommandStateSet, OMX_StateLoaded, NULL);
  if (omx_err != OMX_ErrorNone) {
    LOGE("Videoschd set to loaded state failed");
    exit(1);
  }

  tsem_down(app_priv->rtmpsrc_event_sem);
  LOGI("Rtmpsrc in loaded state");
  tsem_down(app_priv->rtmpout_event_sem);
  LOGI("Rtmpout in loaded state");
  tsem_down(app_priv->clocksrc_event_sem);
  LOGI("Clocksrc in loaded state");
  tsem_down(app_priv->videoschd_event_sem);
  LOGI("Videoschd in loaded state");

  OMX_FreeHandle(app_priv->rtmpsrchandle);
  OMX_FreeHandle(app_priv->rtmpouthandle);
  OMX_FreeHandle(app_priv->clocksrchandle);
  OMX_FreeHandle(app_priv->videoschdhandle);

  OMX_Deinit();

  tsem_reset(app_priv->rtmpsrc_event_sem);
  tsem_reset(app_priv->rtmpout_event_sem);
  tsem_reset(app_priv->clocksrc_event_sem);
  tsem_reset(app_priv->videoschd_event_sem);
  SAFE_FREE(app_priv->rtmpsrc_event_sem);
  SAFE_FREE(app_priv->rtmpout_event_sem);
  SAFE_FREE(app_priv->clocksrc_event_sem);
  SAFE_FREE(app_priv->videoschd_event_sem);

  free(app_priv);
  app_priv = NULL;

  xlog::log_close();
  return 0;
}

OMX_ERRORTYPE rtmpsrc_event_handler(
    OMX_OUT OMX_HANDLETYPE hcomp,
    OMX_OUT OMX_PTR app_data,
    OMX_OUT OMX_EVENTTYPE event,
    OMX_OUT OMX_U32 data1,
    OMX_OUT OMX_U32 data2,
    OMX_OUT OMX_PTR event_data)
{
  appPrivateType *app_priv = (appPrivateType *) app_data;

  if (event == OMX_EventCmdComplete) {
    if (data1 == OMX_CommandStateSet) {
      switch ((int) data2) {
      case OMX_StateInvalid:
        LOGI("Rtmpsrc state changed in OMX_StateInvalid");
        break;
      case OMX_StateLoaded:
        LOGI("Rtmpsrc state changed in OMX_StateLoaded");
        break;
      case OMX_StateIdle:
        LOGI("Rtmpsrc state changed in OMX_StateIdle");
        break;
      case OMX_StateExecuting:
        LOGI("Rtmpsrc state changed in OMX_StateExecuting");
        break;
      case OMX_StatePause:
        LOGI("Rtmpsrc state changed in OMX_StatePause");
        break;
      case OMX_StateWaitForResources:
        LOGI("Rtmpsrc state changed in OMX_StateWaitForResources");
        break;
      }
      tsem_up(app_priv->rtmpsrc_event_sem);
    } else if (data1 == OMX_CommandPortEnable) {
      LOGI("Rtmpsrc received port enable event");
      tsem_up(app_priv->rtmpsrc_event_sem);
    } else if (data1 == OMX_CommandPortDisable) {
      LOGI("Rtmpsrc received port disable event");
      tsem_up(app_priv->rtmpsrc_event_sem);
    } else if (data1 == OMX_CommandFlush) {
      LOGI("Rtmpsrc received flush event");
      tsem_up(app_priv->rtmpsrc_event_sem);
    } else {
      LOGI("Rtmpsrc received event event=%d data1=%u data2=%u", event, data1, data2);
    }
  } else if (event == OMX_EventPortSettingsChanged) {
  } else if (event == OMX_EventPortFormatDetected) {
    LOGI("Rtmpsrc port format detected %x", (int) data1);
  } else if (event == OMX_EventError) {
    LOGE("Rtmpsrc received error event, data1=%x, data2=%d", data1, data2);
  } else {
    LOGI("Rtmpsrc event=%x, data1=%u, data2=%u", event, data1, data2);
  }
  return OMX_ErrorNone;
}

OMX_ERRORTYPE rtmpsrc_fillbuffer_done(
    OMX_OUT OMX_HANDLETYPE hcomp,
    OMX_OUT OMX_PTR app_data,
    OMX_OUT OMX_BUFFERHEADERTYPE *buffer)
{
  return OMX_ErrorNone;
}

OMX_ERRORTYPE rtmpout_event_handler(
    OMX_OUT OMX_HANDLETYPE hcomp,
    OMX_OUT OMX_PTR app_data,
    OMX_OUT OMX_EVENTTYPE event,
    OMX_OUT OMX_U32 data1,
    OMX_OUT OMX_U32 data2,
    OMX_OUT OMX_PTR event_data)
{
  appPrivateType *app_priv = (appPrivateType *) app_data;

  if (event == OMX_EventCmdComplete) {
    if (data1 == OMX_CommandStateSet) {
      switch ((int) data2) {
      case OMX_StateInvalid:
        LOGI("Rtmpout state changed in OMX_StateInvalid");
        break;
      case OMX_StateLoaded:
        LOGI("Rtmpout state changed in OMX_StateLoaded");
        break;
      case OMX_StateIdle:
        LOGI("Rtmpout state changed in OMX_StateIdle");
        break;
      case OMX_StateExecuting:
        LOGI("Rtmpout state changed in OMX_StateExecuting");
        break;
      case OMX_StatePause:
        LOGI("Rtmpout state changed in OMX_StatePause");
        break;
      case OMX_StateWaitForResources:
        LOGI("Rtmpout state changed in OMX_StateWaitForResources");
        break;
      }
      tsem_up(app_priv->rtmpout_event_sem);
    } else if (data1 == OMX_CommandPortEnable) {
      LOGI("Rtmpout received port enable event");
      tsem_up(app_priv->rtmpout_event_sem);
    } else if (data1 == OMX_CommandPortDisable) {
      LOGI("Rtmpout received port disable event");
      tsem_up(app_priv->rtmpout_event_sem);
    } else if (data1 == OMX_CommandFlush) {
      LOGI("Rtmpout received flush event");
      tsem_up(app_priv->rtmpout_event_sem);
    } else {
      LOGI("Rtmpout received event event=%d data1=%u data2=%u", event, data1, data2);
    }
  } else if (event == OMX_EventPortSettingsChanged) {
  } else if (event == OMX_EventPortFormatDetected) {
    LOGI("Rtmpout port format detected %x", (int) data1);
  } else if (event == OMX_EventError) {
    LOGE("Rtmpout received error event, data1=%x, data2=%d", data1, data2);
  } else {
    LOGI("Rtmpout event=%x, data1=%u, data2=%u", event, data1, data2);
  }
  return OMX_ErrorNone;
}

OMX_ERRORTYPE rtmpout_emptybuffer_done(
    OMX_OUT OMX_HANDLETYPE hcomp,
    OMX_OUT OMX_PTR app_data,
    OMX_OUT OMX_BUFFERHEADERTYPE *buffer)
{
  return OMX_ErrorNone;
}

OMX_ERRORTYPE clocksrc_event_handler(
    OMX_OUT OMX_HANDLETYPE hcomp,
    OMX_OUT OMX_PTR app_data,
    OMX_OUT OMX_EVENTTYPE event,
    OMX_OUT OMX_U32 data1,
    OMX_OUT OMX_U32 data2,
    OMX_OUT OMX_PTR event_data)
{
  appPrivateType *app_priv = (appPrivateType *) app_data;

  if (event == OMX_EventCmdComplete) {
    if (data1 == OMX_CommandStateSet) {
      switch ((int) data2) {
      case OMX_StateInvalid:
        LOGI("Clocksrc state changed in OMX_StateInvalid");
        break;
      case OMX_StateLoaded:
        LOGI("Clocksrc state changed in OMX_StateLoaded");
        break;
      case OMX_StateIdle:
        LOGI("Clocksrc state changed in OMX_StateIdle");
        break;
      case OMX_StateExecuting:
        LOGI("Clocksrc state changed in OMX_StateExecuting");
        break;
      case OMX_StatePause:
        LOGI("Clocksrc state changed in OMX_StatePause");
        break;
      case OMX_StateWaitForResources:
        LOGI("Clocksrc state changed in OMX_StateWaitForResources");
        break;
      }
      tsem_up(app_priv->clocksrc_event_sem);
    } else if (data1 == OMX_CommandPortEnable) {
      LOGI("Clocksrc received port enable event");
      tsem_up(app_priv->clocksrc_event_sem);
    } else if (data1 == OMX_CommandPortDisable) {
      LOGI("Clocksrc received port disable event");
      tsem_up(app_priv->clocksrc_event_sem);
    } else if (data1 == OMX_CommandFlush) {
      LOGI("Clocksrc received flush event");
      tsem_up(app_priv->clocksrc_event_sem);
    } else {
      LOGI("Clocksrc received event event=%d data1=%u data2=%u", event, data1, data2);
    }
  } else if (event == OMX_EventPortSettingsChanged) {
  } else if (event == OMX_EventPortFormatDetected) {
    LOGI("Clocksrc port format detected %x", (int) data1);
  } else if (event == OMX_EventError) {
    LOGE("Clocksrc received error event, data1=%x, data2=%d", data1, data2);
  } else {
    LOGI("Clocksrc event=%x, data1=%u, data2=%u", event, data1, data2);
  }
  return OMX_ErrorNone;
}

OMX_ERRORTYPE clocksrc_fillbuffer_done(
    OMX_OUT OMX_HANDLETYPE hcomp,
    OMX_OUT OMX_PTR app_data,
    OMX_OUT OMX_BUFFERHEADERTYPE *buffer)
{
  return OMX_ErrorNone;
}

OMX_ERRORTYPE videoschd_event_handler(
    OMX_OUT OMX_HANDLETYPE hcomp,
    OMX_OUT OMX_PTR app_data,
    OMX_OUT OMX_EVENTTYPE event,
    OMX_OUT OMX_U32 data1,
    OMX_OUT OMX_U32 data2,
    OMX_OUT OMX_PTR event_data)
{
  appPrivateType *app_priv = (appPrivateType *) app_data;

  if (event == OMX_EventCmdComplete) {
    if (data1 == OMX_CommandStateSet) {
      switch ((int) data2) {
      case OMX_StateInvalid:
        LOGI("Videoschd state changed in OMX_StateInvalid");
        break;
      case OMX_StateLoaded:
        LOGI("Videoschd state changed in OMX_StateLoaded");
        break;
      case OMX_StateIdle:
        LOGI("Videoschd state changed in OMX_StateIdle");
        break;
      case OMX_StateExecuting:
        LOGI("Videoschd state changed in OMX_StateExecuting");
        break;
      case OMX_StatePause:
        LOGI("Videoschd state changed in OMX_StatePause");
        break;
      case OMX_StateWaitForResources:
        LOGI("Videoschd state changed in OMX_StateWaitForResources");
        break;
      }
      tsem_up(app_priv->videoschd_event_sem);
    } else if (data1 == OMX_CommandPortEnable) {
      LOGI("Videoschd received port enable event");
      tsem_up(app_priv->videoschd_event_sem);
    } else if (data1 == OMX_CommandPortDisable) {
      LOGI("Videoschd received port disable event");
      tsem_up(app_priv->videoschd_event_sem);
    } else if (data1 == OMX_CommandFlush) {
      LOGI("Videoschd received flush event");
      tsem_up(app_priv->videoschd_event_sem);
    } else {
      LOGI("Videoschd received event event=%d data1=%u data2=%u", event, data1, data2);
    }
  } else if (event == OMX_EventPortSettingsChanged) {
  } else if (event == OMX_EventPortFormatDetected) {
    LOGI("Videoschd port format detected %x", (int) data1);
  } else if (event == OMX_EventError) {
    LOGE("Videoschd received error event, data1=%x, data2=%d", data1, data2);
  } else {
    LOGI("Videoschd event=%x, data1=%u, data2=%u", event, data1, data2);
  }
  return OMX_ErrorNone;
}

OMX_ERRORTYPE videoschd_fillbuffer_done(
    OMX_OUT OMX_HANDLETYPE hcomp,
    OMX_OUT OMX_PTR app_data,
    OMX_OUT OMX_BUFFERHEADERTYPE *buffer)
{
  return OMX_ErrorNone;
}

OMX_ERRORTYPE videoschd_emptybuffer_done(
    OMX_OUT OMX_HANDLETYPE hcomp,
    OMX_OUT OMX_PTR app_data,
    OMX_OUT OMX_BUFFERHEADERTYPE *buffer)
{
  return OMX_ErrorNone;
}