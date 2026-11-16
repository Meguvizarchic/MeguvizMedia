package io.github.jichuo1.meguviz.media;

/** Immutable native observation. CPU_MONOTONIC=2; MEDIA_PTS=1. A GPU submit
 * event records the actual host submit instant, not GPU elapsed/presentation. */
public final class MediaEvent {
    public final long sequence,sessionId,epoch,sourceId,contentId,taskId,allocationId,allocationGeneration;
    public final long domainId,domainGeneration,timestampNs,gpuBatchId;
    public final int kind,timeDomain,status,frameKind;
    MediaEvent(long[] data,int offset){
        sequence=data[offset];kind=(int)data[offset+1];timeDomain=(int)data[offset+2];sessionId=data[offset+3];epoch=data[offset+4];
        sourceId=data[offset+5];contentId=data[offset+6];taskId=data[offset+7];allocationId=data[offset+8];allocationGeneration=data[offset+9];
        domainId=data[offset+10];domainGeneration=data[offset+11];timestampNs=data[offset+12];status=(int)data[offset+13];frameKind=(int)data[offset+14];gpuBatchId=data[offset+15];
    }
    public static final class Batch {
        public final boolean enabled;
        /** Cumulative dropped count since session creation, never an increment. */
        public final long droppedTotal,lastSequence;
        public final MediaEvent[] events;
        Batch(long[] data){
            enabled=data[0]!=0;droppedTotal=data[1];lastSequence=data[2];int count=(int)data[3];
            if(count<0||count>512||data.length!=4+count*16)throw new IllegalStateException("Invalid native event batch");
            events=new MediaEvent[count];for(int i=0;i<count;i++)events[i]=new MediaEvent(data,4+i*16);
        }
    }
}
