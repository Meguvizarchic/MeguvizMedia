package io.github.jichuo1.meguviz.media;

/** Pure timeline facade. All scheduling decisions live in Media core. Objects
 * and returned arrays are copies; proposals carry no native pointer. Explicitly
 * attach the matching SDK before construction. No GPU/model is created here. */
public final class MediaPlanner implements AutoCloseable {
    public static final int ORIGINAL=1, INTERPOLATE=2;
    public static final int DISABLED=1, STALE_LEASE=2, NO_CLOCK=3, PAUSED=4, STALE_CLOCK=5,
        INVALID_TIME=6, IN_FLIGHT=7, AHEAD_OF_WINDOW=8, NO_PREVIOUS_FRAME=9, NO_FUTURE_FRAME=10;
    public enum ClockState { READY, PAUSED, NO_CLOCK, STALE_LEASE, STALE_SNAPSHOT, RESTART_REQUIRED }
    public static final class Config {
        public final int targetFps,frameWindow;
        public final long maxClockAgeNs,presentationDelayNs,maxLeadNs,minLeadNs,originalToleranceUs,maxClockUncertaintyUs;
        public final boolean phaseLock;
        /** maxLeadNs=0 represents an unspecified lead (one target period). */
        public Config(int targetFps,long maxClockAgeNs,long presentationDelayNs,long maxLeadNs,long minLeadNs,
                      long originalToleranceUs,boolean phaseLock,long maxClockUncertaintyUs,int frameWindow) {
            this.targetFps=targetFps;this.maxClockAgeNs=maxClockAgeNs;this.presentationDelayNs=presentationDelayNs;
            this.maxLeadNs=maxLeadNs;this.minLeadNs=minLeadNs;this.originalToleranceUs=originalToleranceUs;
            this.phaseLock=phaseLock;this.maxClockUncertaintyUs=maxClockUncertaintyUs;this.frameWindow=frameWindow;
        }
        public Config(int fps) { this(fps,250_000_000L,0,0,0,0,false,2_000L,4); }
    }
    public static final class Epoch {
        public final long media,decoder,surface,format,discontinuity;
        public Epoch(long media,long decoder,long surface,long format,long discontinuity) {
            this.media=media;this.decoder=decoder;this.surface=surface;this.format=format;this.discontinuity=discontinuity;
        }
    }
    public static final class Clock {
        public final long mediaUs,monotonicNs,discontinuity,uncertaintyUs;
        public final double speed;public final boolean paused;
        public Clock(long mediaUs,long monotonicNs,double speed,boolean paused,long discontinuity,long uncertaintyUs) {
            this.mediaUs=mediaUs;this.monotonicNs=monotonicNs;this.speed=speed;this.paused=paused;
            this.discontinuity=discontinuity;this.uncertaintyUs=uncertaintyUs;
        }
    }
    public static final class Lease {
        private final MediaPlanner owner;public final long generation;public final Epoch epoch;
        private Lease(MediaPlanner owner,long generation,Epoch epoch){this.owner=owner;this.generation=generation;this.epoch=epoch;}
    }
    public static final class Begin {
        public final Lease lease;public final long[] discardedIds;
        private Begin(Lease lease,long[] ids){this.lease=lease;this.discardedIds=ids;}
    }
    public static final class Offered {
        public final boolean accepted;public final long[] discardedIds;
        private Offered(boolean accepted,long[] ids){this.accepted=accepted;this.discardedIds=ids;}
    }
    public static final class ClockUpdate {
        public final ClockState state;public final long[] discardedIds;
        private ClockUpdate(int state,long[] ids){this.state=ClockState.values()[state];this.discardedIds=ids;}
    }
    public static final class Proposal {
        private final MediaPlanner owner;private final long token;
        public final Lease lease;public final long plannedAtNs,dueNs,mediaUs,beforeId,beforePtsUs,afterId,afterPtsUs,uncertaintyUs;
        public final double alpha;public final int mode;
        private Proposal(MediaPlanner owner,Lease lease,long now,long[] data){
            this.owner=owner;this.lease=lease;this.token=data[2];this.plannedAtNs=now;this.dueNs=data[4];this.mediaUs=data[5];
            this.beforeId=data[6];this.beforePtsUs=data[7];this.afterId=data[8];this.afterPtsUs=data[9];
            this.alpha=Double.longBitsToDouble(data[10]);this.uncertaintyUs=data[11];this.mode=(int)data[12];
        }
        public boolean isOriginal(){return mode==ORIGINAL;}
    }
    public static final class Decision {
        public final int waitingReason;public final Long dueNs,mediaUs;public final Proposal proposal;
        private Decision(int reason,Long due,Long media,Proposal proposal){waitingReason=reason;dueNs=due;mediaUs=media;this.proposal=proposal;}
        public boolean isReady(){return proposal!=null;}
    }
    public static final class Counters {
        public final long original,interpolate,snapped,noFuture,noPrevious,skippedLead,skippedBehind;
        private Counters(long[] data){original=data[1];interpolate=data[2];snapped=data[3];noFuture=data[4];noPrevious=data[5];skippedLead=data[6];skippedBehind=data[7];}
    }
    private long handle;private int targetFps;
    public MediaPlanner(Config config){
        if(!MediaRuntime.isAttached())throw new IllegalStateException("Attach the explicit native library first");
        long[] value=nCreate(config.targetFps,config.frameWindow,config.maxClockAgeNs,config.presentationDelayNs,
            config.maxLeadNs,config.minLeadNs,config.originalToleranceUs,config.maxClockUncertaintyUs,config.phaseLock);
        check(value[0]);handle=value[1];targetFps=config.targetFps;
    }
    private static void check(long status){if(status!=0)throw new IllegalStateException("Media planner status "+status);}
    private void open(){if(handle==0)throw new IllegalStateException("Planner is disposed");}
    private long generation(Lease lease){return lease!=null&&lease.owner==this?lease.generation:0;}
    private static long[] tail(long[] source,int start,int count){long[] out=new long[count];System.arraycopy(source,start,out,0,count);return out;}
    public synchronized Begin begin(Epoch epoch){return begin(epoch,targetFps);}
    public synchronized Begin begin(Epoch epoch,int fps){open();long[] a=nBegin(handle,epoch.media,epoch.decoder,epoch.surface,epoch.format,epoch.discontinuity,fps);check(a[0]);targetFps=fps;return new Begin(new Lease(this,a[1],epoch),tail(a,3,(int)a[2]));}
    /** Ends only the matching generation; the planner remains reusable. */
    public synchronized long[] closeEpoch(Lease lease){if(handle==0)return new long[0];long[] a=nCloseEpoch(handle,generation(lease));check(a[0]);return tail(a,2,(int)a[1]);}
    public synchronized Offered offer(Lease lease,long frameId,long ptsUs){open();long[] a=nOffer(handle,generation(lease),frameId,ptsUs);check(a[0]);return new Offered(a[1]!=0,tail(a,3,(int)a[2]));}
    public synchronized ClockUpdate updateClock(Lease lease,Clock clock){open();long[] a=nClock(handle,generation(lease),clock!=null,clock==null?0:clock.mediaUs,clock==null?0:clock.monotonicNs,clock==null?1:clock.speed,clock!=null&&clock.paused,clock==null?0:clock.discontinuity,clock==null?0:clock.uncertaintyUs);check(a[0]);return new ClockUpdate((int)a[1],tail(a,3,(int)a[2]));}
    public synchronized ClockUpdate updateClockFromHost(Lease lease,HostBridge host){HostBridge.ClockSnapshot c=host.clock();return updateClock(lease,c==null?null:new Clock(c.mediaUs,c.monotonicNs,c.speed,c.paused,c.discontinuity,c.uncertaintyUs));}
    public synchronized Decision plan(Lease lease,long nowNs){open();long[] a=nPlan(handle,generation(lease),nowNs);check(a[0]);int reason=(int)a[1];if(reason==0)return new Decision(0,a[4],a[5],new Proposal(this,lease,nowNs,a));boolean due=reason==IN_FLIGHT||reason==AHEAD_OF_WINDOW||reason==NO_PREVIOUS_FRAME||reason==NO_FUTURE_FRAME;boolean media=reason==IN_FLIGHT||reason==NO_PREVIOUS_FRAME||reason==NO_FUTURE_FRAME;return new Decision(reason,due?a[4]:null,media?a[5]:null,null);}
    public synchronized boolean valid(Proposal proposal,long nowNs){return handle!=0&&proposal!=null&&proposal.owner==this&&nCheck(handle,proposal.token,generation(proposal.lease),nowNs,false)==0;}
    public synchronized boolean commit(Proposal proposal,long nowNs){return handle!=0&&proposal!=null&&proposal.owner==this&&nCheck(handle,proposal.token,generation(proposal.lease),nowNs,true)==0;}
    public synchronized long effectiveDelayUs(){open();long[] a=nDelay(handle,1,true);check(a[0]);return a[1];}
    public synchronized long effectiveDelayUs(double speed){open();long[] a=nDelay(handle,speed,false);check(a[0]);return a[1];}
    public synchronized Counters counters(){open();long[] a=nCounters(handle);check(a[0]);return new Counters(a);}
    /** Final disposal; closeEpoch is the reusable epoch operation. */
    @Override public synchronized void close(){if(handle!=0){nRelease(handle);handle=0;}}
    private static native long[] nCreate(int fps,int window,long maxAge,long delay,long maxLead,long minLead,long tolerance,long uncertainty,boolean lockPhase);
    private static native long[] nBegin(long handle,long media,long decoder,long surface,long format,long discontinuity,int fps);
    private static native long[] nCloseEpoch(long handle,long generation);
    private static native long[] nOffer(long handle,long generation,long frame,long pts);
    private static native long[] nClock(long handle,long generation,boolean present,long media,long monotonic,double speed,boolean paused,long discontinuity,long uncertainty);
    private static native long[] nPlan(long handle,long generation,long now);
    private static native int nCheck(long handle,long token,long generation,long now,boolean commit);
    private static native long[] nDelay(long handle,double speed,boolean fromClock);
    private static native long[] nCounters(long handle);
    private static native void nRelease(long handle);
}
