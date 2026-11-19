package io.github.jichuo1.meguviz.media;

import android.os.Handler;
import android.os.Looper;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

/** OES capture and presentation on the creating GL worker/context. Model bytes
 * are explicitly loaded through MediaNative before construction. The engine's
 * domain/model are retained independently; no decoder Surface is replaced.
 * Captured pixels use the requested processing extent and canonical top-down
 * RGBA8 SDR/full-range rows. The
 * source adapter must check its actual color contract before using this API. */
public final class AndroidMediaSession implements AutoCloseable {
    public static final int AHB_HOST_WAIT = 1, AHB_GPU_SYNC = 2;
    public static final int SOURCE_AVAILABLE = 1, SOURCE_LATCHED = 2, ALGORITHM_ACCEPTED = 5,
        SINK_ACCEPTED = 6, ACTUAL_PRESENTED = 7, OUTPUT_RELEASED = 9;
    private final Thread owner = Thread.currentThread();
    private final HostBridge host;
    private long handle, epoch = 1;
    private boolean paused;
    private boolean retirementWakePending, nativeCloseCompleted;
    private MediaPerformanceHint performanceHint;
    // At most one unpublished Java result is allowed at a time. If native
    // retirement is saturated, retain its token until owner-thread retry.
    private long pendingJavaFrameRelease;
    // Installed before entering JNI, including its preallocated receipt and
    // optional handler. If scheduling fails the owner remains retrievable by
    // this exact thread through servicePendingCreation().
    private static final ThreadLocal<PendingCreation> pendingCreation = new ThreadLocal<>();
    private static final class PendingCreation implements Runnable {
        final long[] receipt = new long[2];
        final Handler handler;
        Preparation preparation;
        boolean sessionClosed, scheduled;
        PendingCreation() { Looper looper = Looper.myLooper(); handler = looper == null ? null : new Handler(looper); }
        synchronized void schedule() {
            if (handler == null || scheduled) return;
            try { scheduled = handler.postDelayed(this, 32); }
            catch (RuntimeException | OutOfMemoryError ignored) { scheduled = false; }
        }
        @Override public void run() {
            synchronized(this){scheduled = false;}
            if (pendingCreation.get() != this) return;
            try { if (servicePendingCreation() != 0) schedule(); }
            catch (RuntimeException | LinkageError | OutOfMemoryError ignored) { schedule(); }
        }
    }

    /** Request background code-pin preparation before a GL-owner create.
     * This starts preparation asynchronously and does not promise readiness. */
    public static boolean isCreationProtocolSupported() {
        return MediaRuntime.isAttached() && MediaNative.androidCreationProtocolVersion() == 2;
    }
    public static void prepareCreationAsync() {
        if (!MediaRuntime.isAttached()) throw new IllegalStateException("Attach the validated library first");
        if (!isCreationProtocolSupported()) throw new LinkageError("Unsupported Android Media creation protocol");
        MediaRetirementService.prepareCreation();
    }
    /** Release unused prepared pins on the background retirement worker. */
    public static void discardPreparedCreationAsync() { MediaRetirementService.discardCreation(); }
    private static boolean requestRetirementWake() {
        try {
            MediaRetirementService.requestImmediate();
            return MediaRetirementService.status() != 9;
        } catch (Throwable ignored) { return false; }
    }
    /** Advance an abandoned creation on its original GL thread and context.
     * Zero means no GL-owner work remains for this thread; background loader
     * cleanup may still be pending. BUSY/TIMEOUT requires another call. The
     * host must retain that thread/context until 0 and its independent library
     * reference through the background service's completion. */
    public static int servicePendingCreation() {
        PendingCreation pending = pendingCreation.get();
        int platform = MediaNative.androidServiceCreationOwner();
        if (pending == null) return requestRetirementWake() ? platform : (platform == 0 ? -7 : platform);
        long receipt = pending.receipt[1];
        if (receipt != 0 && !pending.sessionClosed) {
            int status = MediaNative.androidAbandonCreation(receipt);
            if (status == 0) pending.sessionClosed = true;
            else { requestRetirementWake(); return status; }
        }
        if (!requestRetirementWake()) return platform == 0 ? -7 : platform;
        if (platform != 0) return platform;
        if (pending.preparation != null) pending.preparation.ownerRetired = true;
        pendingCreation.remove();
        return 0;
    }

    private static final class PreparationWorker {
        static final ThreadPoolExecutor EXECUTOR = new ThreadPoolExecutor(1, 1, 2, TimeUnit.SECONDS,
            new ArrayBlockingQueue<>(1), task -> {
                Thread thread = new Thread(task, "Meguviz-session-prepare"); thread.setDaemon(true); return thread;
            }, new ThreadPoolExecutor.AbortPolicy());
        static { EXECUTOR.allowCoreThreadTimeOut(true); }
    }
    /** A bounded asynchronous plan/import owner. Cancellation is advisory:
     * do not release the caller's engine token until retirementStatus()==0. */
    public static final class Preparation {
        public static final int PREPARING_PLAN=1, READY_TO_BEGIN=2, IMPORTING=3, READY_TO_COMMIT=4,
            CANCELLED=5, FAILED=6, COMMITTED=7, BEGINNING=8, COMMITTING=9;
        private final AtomicInteger state = new AtomicInteger(PREPARING_PLAN), workers = new AtomicInteger();
        // 0=new, 1=reserve in flight, 2=start finished, 3=retired before start.
        private final AtomicInteger startPhase = new AtomicInteger();
        private final long[] receipt = new long[2];
        private volatile long receiptId;
        private final long engine;
        private final int width,height,sourceWidth,sourceHeight,capacity,path;
        private final boolean admission;
        private final HostBridge host;
        private final Runnable planWork, importWork;
        private volatile int error;
        private volatile boolean ownerRetired = true, unknownReceipt;
        private volatile PendingCreation ownerPending;
        private Preparation(long engine,int width,int height,int sourceWidth,int sourceHeight,int capacity,int path,boolean admission,HostBridge host) {
            this.engine=engine;this.width=width;this.height=height;this.sourceWidth=sourceWidth;this.sourceHeight=sourceHeight;
            this.capacity=capacity;this.path=path;this.admission=admission;this.host=host;
            // Both task objects exist before native reservation and device work.
            planWork=() -> runPlan();importWork=() -> runImport();
        }
        public int status(){return state.get();}
        public int errorStatus(){return error;}
        /** Begin only after the caller has retained this object. Once native
         * reservation begins every failure remains observable through it. */
        public int start(){
            if(!startPhase.compareAndSet(0,1))return -7;
            try{
            if(state.get()!=PREPARING_PLAN)return -13;
            int status;
            try{status=MediaNative.androidAsyncReserve(receipt);}
            catch(Throwable failure){
                receiptId=receipt[1];
                error=failure instanceof OutOfMemoryError?-10:-14;state.compareAndSet(PREPARING_PLAN,FAILED);
                if(receiptId!=0)try{MediaNative.androidAsyncCancel(receiptId);}catch(Throwable ignored){}
                try{MediaRetirementService.requestImmediate();}catch(Throwable ignored){}return error;
            }
            receiptId=receipt[1];
            if(status!=0){error=status;state.compareAndSet(PREPARING_PLAN,FAILED);
                if(receiptId!=0)try{MediaNative.androidAsyncCancel(receiptId);}catch(Throwable ignored){}
                try{MediaRetirementService.requestImmediate();}catch(Throwable ignored){}return status;}
            if(receiptId==0){unknownReceipt=true;error=-14;state.compareAndSet(PREPARING_PLAN,FAILED);
                try{MediaRetirementService.requestImmediate();}catch(Throwable ignored){}return error;}
            // Cancellation may have won while reserve was in flight; its
            // first cancel had no receipt. Repeat it after publication.
            if(state.get()!=PREPARING_PLAN){try{MediaNative.androidAsyncCancel(receiptId);}catch(Throwable ignored){}
                try{MediaRetirementService.requestImmediate();}catch(Throwable ignored){}return -13;}
            if(!submit(planWork)){error=-8;state.compareAndSet(PREPARING_PLAN,FAILED);
                try{MediaNative.androidAsyncCancel(receiptId);}catch(Throwable ignored){}
                try{MediaRetirementService.requestImmediate();}catch(Throwable ignored){}return -8;}
            return 0;
            }finally{startPhase.set(2);}
        }
        private boolean submit(Runnable work){
            workers.incrementAndGet();
            try { PreparationWorker.EXECUTOR.execute(work); return true; }
            catch (Throwable failure) { workers.decrementAndGet(); return false; }
        }
        private void finishWorker(){workers.decrementAndGet();requestRetirementWake();}
        private void runPlan(){
            try {
                // Start the independent service before entering a potentially
                // unbounded native driver call; it retries while creating=true.
                MediaRetirementService.requestImmediate();
                int status=MediaNative.androidAsyncPreparePlan(receiptId,engine,width,height,sourceWidth,sourceHeight,capacity,path,admission);
                if(status==0){if(!state.compareAndSet(PREPARING_PLAN,READY_TO_BEGIN))MediaNative.androidAsyncCancel(receiptId);}
                else{error=status;state.compareAndSet(PREPARING_PLAN,status==-13?CANCELLED:FAILED);}
            }catch(RuntimeException|LinkageError|OutOfMemoryError failure){error=-14;state.compareAndSet(PREPARING_PLAN,FAILED);try{MediaNative.androidAsyncCancel(receiptId);}catch(Throwable ignored){}}
            finally{finishWorker();}
        }
        private void runImport(){
            try {
                MediaRetirementService.requestImmediate();
                int status=MediaNative.androidAsyncImport(receiptId);
                if(status==0){if(!state.compareAndSet(IMPORTING,READY_TO_COMMIT))MediaNative.androidAsyncCancel(receiptId);}
                else{error=status;state.compareAndSet(IMPORTING,status==-13?CANCELLED:FAILED);}
            }catch(RuntimeException|LinkageError|OutOfMemoryError failure){error=-14;state.compareAndSet(IMPORTING,FAILED);try{MediaNative.androidAsyncCancel(receiptId);}catch(Throwable ignored){}}
            finally{finishWorker();if(state.get()!=READY_TO_COMMIT&&ownerPending!=null)try{ownerPending.schedule();}catch(Throwable ignored){}}
        }
        /** Called only on the intended GL owner/context; never waits for plan or import. */
        public int beginOnGl(){
            if(!state.compareAndSet(READY_TO_BEGIN,BEGINNING))return -7;
            if(pendingCreation.get()!=null){state.compareAndSet(BEGINNING,READY_TO_BEGIN);return -7;}
            PendingCreation pending=null;
            try{pending=new PendingCreation();pending.receipt[1]=receiptId;pending.preparation=this;pendingCreation.set(pending);}
            catch(RuntimeException|OutOfMemoryError failure){
                // ThreadLocal.set can publish the entry and then fail while
                // growing its table. No native GL operation has started, but
                // retrying BEGINNING would strand a partly installed receipt.
                // Cancel the unbound session and keep the holder reachable;
                // retirementStatus still waits for the native registry.
                error=failure instanceof OutOfMemoryError?-10:-14;
                state.compareAndSet(BEGINNING,FAILED);
                ownerPending=pending;ownerRetired=true;
                try{if(receiptId!=0)MediaNative.androidAsyncCancel(receiptId);}catch(Throwable ignored){}
                try{MediaRetirementService.requestImmediate();}catch(Throwable ignored){}
                if(pending!=null)pending.schedule();
                return error;
            }
            ownerPending=pending;ownerRetired=false;
            int status;
            try{status=MediaNative.androidAsyncBeginGl(receiptId);}
            catch(RuntimeException|LinkageError|OutOfMemoryError failure){status=-14;}
            if(status==0&&state.compareAndSet(BEGINNING,IMPORTING)){
                if(submit(importWork))return 0;
                error=-8;state.compareAndSet(IMPORTING,FAILED);
            }else if(status!=0){error=status;state.compareAndSet(BEGINNING,status==-13?CANCELLED:FAILED);}
            try{MediaNative.androidAsyncCancel(receiptId);}catch(Throwable ignored){}
            try{if(servicePendingCreation()!=0)pending.schedule();}catch(Throwable ignored){pending.schedule();}
            return error!=0?error:-13;
        }
        /** Called on the same GL owner after READY_TO_COMMIT; null means retry. */
        public AndroidMediaSession commitOnGl(){
            if(!state.compareAndSet(READY_TO_COMMIT,COMMITTING))return null;
            int status;
            try{status=MediaNative.androidAsyncCommitGl(receiptId);}
            catch(RuntimeException|LinkageError|OutOfMemoryError failure){status=-14;}
            if(status==-7||status==-12){state.compareAndSet(COMMITTING,READY_TO_COMMIT);return null;}
            if(status!=0){error=status;state.compareAndSet(COMMITTING,status==-13?CANCELLED:FAILED);
                try{if(servicePendingCreation()!=0&&ownerPending!=null)ownerPending.schedule();}catch(Throwable ignored){}return null;}
            try{
                AndroidMediaSession result=new AndroidMediaSession(receiptId,host);
                if(state.compareAndSet(COMMITTING,COMMITTED)){pendingCreation.remove();ownerPending=null;ownerRetired=true;return result;}
            }catch(RuntimeException|OutOfMemoryError failure){error=-10;state.compareAndSet(COMMITTING,FAILED);}
            try{MediaNative.androidAsyncCancel(receiptId);}catch(Throwable ignored){}
            try{if(servicePendingCreation()!=0&&ownerPending!=null)ownerPending.schedule();}catch(Throwable ignored){}
            return null;
        }
        /** Nonblocking request; BUSY or CANCELLED does not mean physical retirement. */
        public int cancel(){
            for(;;){int old=state.get();if(old==COMMITTED)return -7;if(old==CANCELLED||old==FAILED)break;
                if(state.compareAndSet(old,CANCELLED))break;}
            int status;
            try{status=receiptId==0?0:MediaNative.androidAsyncCancel(receiptId);}catch(RuntimeException|LinkageError|OutOfMemoryError failure){status=-14;}
            try{if(PreparationWorker.EXECUTOR.remove(planWork))workers.decrementAndGet();}catch(Throwable ignored){}
            try{if(PreparationWorker.EXECUTOR.remove(importWork))workers.decrementAndGet();}catch(Throwable ignored){}
            try{if(ownerPending!=null)ownerPending.schedule();}catch(Throwable ignored){}
            requestRetirementWake();return status;
        }
        /** Zero with a receipt requires all JNI workers returned, GL-owner
         * cleanup completed, and the native Session removed. Without a
         * receipt, zero means no engine-referencing work was started; an
         * unreported guard-only reservation may still retire in background.
         * A cancellation signal alone is not proof. */
        public int retirementStatus(){
            // No receipt means reserve ended before any plan, GL, or import
            // work could be scheduled. It cannot retain the candidate engine.
            int phase=startPhase.get();
            if(phase==0){if(startPhase.compareAndSet(0,3))return 0;phase=startPhase.get();}
            if(phase==1)return -7;if(phase==3)return 0;
            if(receiptId==0)return unknownReceipt?-14:(workers.get()==0&&ownerRetired?0:-7);
            int current=state.get();
            if(current==CANCELLED||current==FAILED){
                try{MediaNative.androidAsyncCancel(receiptId);MediaRetirementService.requestImmediate();}
                catch(RuntimeException|LinkageError|OutOfMemoryError ignored){return -14;}
            }
            if(workers.get()!=0||!ownerRetired)return -7;
            try{return MediaNative.androidAsyncRetirementStatus(receiptId);}
            catch(RuntimeException|LinkageError|OutOfMemoryError failure){return -14;}
        }
    }
    /** Allocate a reviewable preparation without reserving native resources. */
    public static Preparation newPreparation(long engine,int width,int height,int sourceWidth,int sourceHeight,
                                               int capacity,int path,boolean admission,HostBridge host){
        if(!isCreationProtocolSupported())throw new LinkageError("Unsupported Android Media creation protocol");
        ThreadPoolExecutor worker=PreparationWorker.EXECUTOR; // initialize before native reservation
        return new Preparation(engine,width,height,sourceWidth,sourceHeight,capacity,path,admission,host);
    }
    /** Convenience form; callers needing ownership before any native entry
     * should retain newPreparation(...) and invoke start() explicitly. */
    public static Preparation prepareAsync(long engine,int width,int height,int sourceWidth,int sourceHeight,
                                           int capacity,int path,boolean admission,HostBridge host){
        Preparation preparation=newPreparation(engine,width,height,sourceWidth,sourceHeight,capacity,path,admission,host);
        preparation.start();return preparation;
    }

    private AndroidMediaSession(long existingHandle,HostBridge host){this.host=host;this.handle=existingHandle;}

    public AndroidMediaSession(long loadedEngine, int width, int height, int poolCapacity,
                               int path, boolean deadlineAdmission, HostBridge host) {
        this(loadedEngine, width, height, width, height, poolCapacity, path, deadlineAdmission, host);
    }
    /** Source dimensions select the original four-bilinear-sample filter when
     * shrinking by at least 1.5x. Raw SurfaceTexture matrices stay unchanged. */
    public AndroidMediaSession(long loadedEngine, int width, int height, int sourceWidth, int sourceHeight,
                               int poolCapacity, int path, boolean deadlineAdmission, HostBridge host) {
        if (!MediaRuntime.isAttached()) throw new IllegalStateException("Attach the validated library first");
        if (!isCreationProtocolSupported()) throw new LinkageError("Unsupported Android Media creation protocol");
        this.host = host;
        if (pendingCreation.get() != null && servicePendingCreation() != 0) throw new MediaException(-7);
        PendingCreation attempt = new PendingCreation();
        pendingCreation.set(attempt);
        boolean committed = false;
        try {
            int status = MediaNative.androidCreatePrepared(loadedEngine, width, height, sourceWidth, sourceHeight,
                poolCapacity, path, deadlineAdmission, attempt.receipt);
            check(status); handle = attempt.receipt[1]; committed = true;
        } finally {
            if (committed) pendingCreation.remove();
            else {
                try { if (servicePendingCreation() != 0) attempt.schedule(); }
                catch (RuntimeException | LinkageError | OutOfMemoryError ignored) { attempt.schedule(); }
            }
            requestRetirementSafely();
        }
    }
    /** Independent model-free client for the same OES/AHB/GLES content bridge. */
    public static AndroidMediaSession identity(int width, int height, int poolCapacity, int path, HostBridge host) {
        return new AndroidMediaSession(0, width, height, poolCapacity, path, false, host);
    }
    private void owner() {
        if (Thread.currentThread() != owner) throw new IllegalStateException("Use the creating GL worker");
        if (handle == 0 || nativeCloseCompleted) throw new IllegalStateException("Session is closed");
        if (retirementWakePending) requestRetirementSafely();
    }
    // Scheduling is auxiliary: an exception here must never hide an already
    // published JNI frame token or replay a completed native close. Retain a
    // retry flag if the scheduler reports or throws a failure.
    private void requestRetirementSafely() {
        retirementWakePending=!requestRetirementWake();
    }
    private static void check(int status) {
        if (status != 0) throw new MediaException(status);
    }
    private void retryPendingJavaFrameRelease() {
        if (pendingJavaFrameRelease == 0) return;
        int status=MediaNative.androidReleaseFrameDeferred(handle,pendingJavaFrameRelease);
        if(status==0||status==1||status==-7){
            pendingJavaFrameRelease=0;
            if(status!=0)requestRetirementSafely();
            return;
        }
        requestRetirementSafely();
        throw new MediaException(status);
    }
    private void abandonUnpublishedFrame(long token) {
        if(token==0)return;
        pendingJavaFrameRelease=token;
        try{retryPendingJavaFrameRelease();}catch(RuntimeException|Error ignored){}
        requestRetirementSafely();
    }
    public static final class MediaException extends IllegalStateException {
        public final int status;
        private MediaException(int status) { super("Media status " + status); this.status = status; }
    }
    /** Probe fields follow MgmAndroidProbe, with status as the first element.
     * workloadQualified remains zero until a separately recorded device gate. */
    public long[] probe() { owner(); return MediaNative.androidProbe(handle); }
    /** Actual immutable RGBA8 plan JSON, or an empty string if unavailable
     * (including model-free identity sessions). This is not per-job data. */
    public String executionInfo() { owner(); return MediaNative.androidExecutionInfo(handle); }
    /** Optional snapshot owned by this exact completed RIFE frame. Returns a
     * single nonzero status when unavailable. On success the fields are:
     * status, request ID, RIFE frame kind, effective and cache-lookup scale,
     * static/motion guard dispatch, left/right tensor and bridge cache states,
     * tensor publication observed/count, left/right source ID, and epoch.
     * This call does not imply presentation or per-pixel guard effects. */
    public long[] jobObservation(Frame frame) {
        owner(); return MediaNative.androidFrameJobObservation(handle, valid(frame));
    }
    public long epoch() { return epoch; }
    public void setPaused(boolean value) { owner(); paused = value; }
    public void beginEpoch(long next) {
        owner(); if (next <= epoch) throw new IllegalArgumentException("Epoch must increase");
        check(MediaNative.androidBeginEpoch(handle, next)); epoch = next;
    }
    /** Pass the raw SurfaceTexture/crop/rotation transform. The platform applies
     * the canonical vertical flip before it, matching the original capture, and
     * flips top-down rows back during GL presentation. Do not add a second flip.
     * Caller owns the original OES producer lease until this call returns. */
    public Frame capture(int oesTexture, float[] transform, long sourceId, long ptsUs, int primaries, int matrix) {
        owner(); retryPendingJavaFrameRelease(); checkHostPause(); if (paused) throw new IllegalStateException("Session is paused");

        long[] result;
        try { result = MediaNative.androidCapture(handle, oesTexture, transform, sourceId, epoch, ptsUs, primaries, matrix); }
        finally { requestRetirementSafely(); }
        check((int)result[0]); long token=result[1];
        try{return new Frame(token,epoch);}catch(RuntimeException|Error failure){abandonUnpublishedFrame(token);throw failure;}
    }
    /** Returns [status, isStatic, isCut, readbackBytes, isDuplicate].
     * The byte count includes atlas padding, and counts shared grids once per input. */
    public long[] compare(Frame left, Frame right) { owner(); return MediaNative.androidCompare(handle, valid(left), valid(right)); }
    /** Runs on this dedicated worker; an accepted result is delivered only after
     * physical retirement and algorithm guards. Deadline admission and stale
     * output rejection are independently enforced. */
    public Frame interpolate(Frame left, Frame right, double alpha, long deadlineNs) {
        owner(); retryPendingJavaFrameRelease(); checkHostPause(); if (paused) throw new IllegalStateException("Session is paused");
        long[] result;
        try { result = MediaNative.androidInterpolate(handle, valid(left), valid(right), alpha, deadlineNs); }
        catch (RuntimeException | Error failure) { requestRetirementSafely(); throw failure; }
        check((int)result[0]); if (result[1] == 0) throw new MediaException(-14);
        long token=result[1];
        try{return new Frame(token,epoch);}catch(RuntimeException|Error failure){abandonUnpublishedFrame(token);throw failure;}
    }
    /** Nonblocking effect submission. Callback work stays native and publishes
     * into a bounded completion queue after physical retirement. */
    public long submit(Frame left, Frame right, double alpha, long deadlineNs) {
        return submit(left, right, alpha, deadlineNs, 0);
    }
    public long submit(Frame left, Frame right, double alpha, long deadlineNs, int flowScale) {
        owner(); checkHostPause(); if (paused) throw new IllegalStateException("Session is paused");
        long[] result = MediaNative.androidSubmit(handle, valid(left), valid(right), alpha, deadlineNs, flowScale);
        if (result[0] != 1) throw new MediaException(result[0] < 0 ? (int)result[0] : -14);
        if (result[1] == 0) throw new MediaException(-14); return result[1];
    }
    public final class Completed {
        public final int status;
        public final long jobId;
        public final Frame frame;
        public final long epoch;
        public final int frameKind;
        public final int unreliablePermille, histogramPermille, patchedTiles, effectiveFlowScale;
        public final long[] stageNanos;
        public final long workerCpuNanos;
        public final boolean workerCpuObserved;
        /** {observed,elapsedNs,batches,observedBatches,validBits,periodFloatBits,eventCoverage}. */
        public final long[] gpuMeasurement;
        public final long workPeriodStartUptimeNanos, workPeriodDurationNanos;
        public final int workerThreadId;
        private Completed(int status, long jobId, Frame frame, long[] result) {
            this.status=status; this.jobId=jobId; this.frame=frame;
            epoch=result.length>10?result[10]:0;
            frameKind=result.length>11?(int)result[11]:0;
            unreliablePermille=result.length>3?(int)result[3]:0;
            histogramPermille=result.length>4?(int)result[4]:0;
            patchedTiles=result.length>5?(int)result[5]:0;
            effectiveFlowScale=result.length>6?(int)result[6]:0;
            stageNanos=result.length>9?new long[]{result[7],result[8],result[9]}:new long[3];
            workerCpuObserved=result.length>13&&result[13]!=0;
            workerCpuNanos=workerCpuObserved?result[12]:-1;
            gpuMeasurement=result.length>20?new long[]{result[14],result[15],result[16],result[17],result[18],result[19],result[20]}:new long[7];
            workPeriodStartUptimeNanos=result.length>21?result[21]:0;
            workPeriodDurationNanos=result.length>22?result[22]:0;
            workerThreadId=result.length>23?(int)result[23]:0;
        }
    }
    /** null means no retired completion. Failed/cancelled jobs carry no frame. */
    public Completed pollCompleted() {
        owner(); retryPendingJavaFrameRelease(); long[] result;
        try { result=MediaNative.androidPollCompleted(handle); }
        catch (RuntimeException | Error failure) { requestRetirementSafely(); throw failure; }
        if (result[0] == -7) return null;
        long token=result.length>11?result[2]:0;
        try{
            Frame frame=token!=0?new Frame(token,result[10]):null;
            Completed completed=new Completed((int)result[0],result.length>1?result[1]:0,frame,result);
            if(performanceHint!=null)performanceHint.report(completed);
            return completed;
        }catch(RuntimeException|Error failure){abandonUnpublishedFrame(token);throw failure;}
    }
    /** Explicit P3 experiment. API 35+ reports only complete native measurements; older versions stay disabled. */
    public void setAdpfExperimentEnabled(android.content.Context context, long targetWorkDurationNanos, boolean enabled) {
        owner();
        if (performanceHint != null) { performanceHint.close(); performanceHint = null; }
        if (enabled) performanceHint = MediaPerformanceHint.create(context, targetWorkDurationNanos);
    }
    public int cancel(long jobId) { owner(); return MediaNative.androidCancel(handle, jobId); }
    /** Default off. The native ring holds at most 512 copied records. */
    public void setEventCollectionEnabled(boolean enabled) { owner(); check(MediaNative.androidSetEventsEnabled(handle,enabled)); }
    public MediaEvent.Batch pollEvents() {
        owner();MediaEvent.Batch batch=new MediaEvent.Batch(MediaNative.androidPollEvents(handle));
        if(host!=null){for(MediaEvent event:batch.events)host.onNativeMediaEvent(event);host.onNativeEventsDropped(batch.droppedTotal);}
        return batch;
    }
    private void checkHostPause() { if(host!=null){HostBridge.ClockSnapshot clock=host.clock();if(clock!=null&&clock.paused)throw new MediaException(-13);} }
    /** Caller performs its own eglSwapBuffers after this draw. Success means
     * sink accepted, and does not mean actual display/presentation. */
    public void draw(Frame frame, int width, int height) {
        owner(); checkHostPause(); if (paused) throw new IllegalStateException("Session is paused");
        check(MediaNative.androidDraw(handle, valid(frame), width, height));
    }
    public int poll(long timeoutNs) {
        owner(); int status=MediaNative.androidPoll(handle, timeoutNs);
        if (status == -7 || status == -12 || status == -11) requestRetirementSafely();
        return status;
    }
    /** Sticky state of this exact device generation. Returns 0 if no loss was
     * observed, -11 for device loss, or another negative status if the query
     * failed. Does not wait for Vulkan/GL/producer retirement or close handles. */
    public int deviceLossStatus() {
        owner(); int status=MediaNative.androidDeviceLossStatus(handle);
        if (status == -11) requestRetirementSafely();
        return status;
    }
    /** {status, domainId, generation, deviceEpoch, lost, deviceRetired}; a
     * nonzero status means the remaining fields cannot establish a live state. */
    public long[] deviceState() { owner(); return MediaNative.androidDeviceState(handle); }
    private long valid(Frame frame) {
        if (frame == null || frame.session() != this || frame.id == 0 || frame.epoch != epoch)
            throw new IllegalArgumentException("Missing or stale frame");
        return frame.id;
    }
    private void event(int kind, long sourceId, long frameId) {
        if (host != null) host.onMediaEvent(kind, epoch, sourceId, frameId, System.nanoTime());
    }
    @Override public void close() {
        if (Thread.currentThread() != owner) throw new IllegalStateException("Use the creating GL worker");
        if (handle == 0) throw new IllegalStateException("Session is closed");
        if (!nativeCloseCompleted) {
            retryPendingJavaFrameRelease();
            if (performanceHint != null) { performanceHint.close(); performanceHint = null; }
            int status;
            try { status = MediaNative.androidClose(handle); }
            finally { requestRetirementSafely(); }
            if (status == 0) nativeCloseCompleted=true;
            check(status);
        } else if (retirementWakePending) requestRetirementSafely();
        // The native registry has already been retired. If its background
        // wake could not be scheduled, retain the Java owner and retry only
        // the wake; never call androidClose on the erased registry again.
        if (retirementWakePending) throw new MediaException(-7);
        handle = 0;
    }
    public final class Frame implements AutoCloseable {
        private long id;
        public final long epoch;
        private Frame(long id, long epoch) { this.id = id; this.epoch = epoch; }
        private AndroidMediaSession session() { return AndroidMediaSession.this; }
        @Override public void close() {
            owner(); if (id != 0) {
                int status=MediaNative.androidReleaseFrameDeferred(handle, id);
                if(status==1 || status==-7)requestRetirementSafely();
                else check(status);
                id = 0;
            }
        }
    }
}
