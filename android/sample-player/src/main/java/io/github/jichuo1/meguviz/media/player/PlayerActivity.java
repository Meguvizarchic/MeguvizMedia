package io.github.jichuo1.meguviz.media.player;

import android.app.Activity;
import android.content.Intent;
import android.graphics.SurfaceTexture;
import android.media.MediaCodec;
import android.media.MediaExtractor;
import android.media.MediaFormat;
import android.net.Uri;
import android.opengl.EGL14;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.GLES11Ext;
import android.opengl.GLES30;
import android.opengl.GLSurfaceView;
import android.os.Bundle;
import android.view.Surface;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;
import io.github.jichuo1.meguviz.media.AndroidMediaSession;
import io.github.jichuo1.meguviz.media.MediaRuntime;
import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.concurrent.atomic.AtomicBoolean;
import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;

/** Independent MediaCodec → OES → owned full-resolution AHB → GLES identity
 * client. Video only: this sample does not claim audio synchronization, actual
 * display feedback, RIFE inference, or qualified performance. */
public final class PlayerActivity extends Activity {
    private GLSurfaceView view;
    private TextView status;
    private final Renderer renderer = new Renderer();
    private final DecoderGate decoder = new DecoderGate();
    private final DeferredStart<Uri> pendingStart = new DeferredStart<>();
    private final DeferredRetry startRetry = new DeferredRetry(), retirementRetry = new DeferredRetry(), completionRetry = new DeferredRetry();
    private volatile boolean resumed;
    private Uri selectedSource;
    private boolean resumeSelected;
    @Override public void onCreate(Bundle saved) {
        super.onCreate(saved);
        LinearLayout layout = new LinearLayout(this); layout.setOrientation(LinearLayout.VERTICAL);
        Button choose = new Button(this); choose.setText("选择 SDR 视频（Identity / 无音频）");
        choose.setOnClickListener(v -> startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT)
            .addCategory(Intent.CATEGORY_OPENABLE).setType("video/*"), 1));
        status = new TextView(this); status.setText("待选择视频；AHB 能力以实际 probe 为准");
        view = new GLSurfaceView(this); view.setEGLContextClientVersion(3); view.setPreserveEGLContextOnPause(true);
        view.setRenderer(renderer); view.setRenderMode(GLSurfaceView.RENDERMODE_WHEN_DIRTY);
        layout.addView(choose); layout.addView(status); layout.addView(view, new LinearLayout.LayoutParams(-1, 0, 1));
        setContentView(layout);
    }
    private void report(String text) { runOnUiThread(() -> status.setText(text)); }
    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != 1 || result != RESULT_OK || data == null || data.getData() == null) return;
        // Publish the new request without waiting on the UI thread. The GL
        // consumer retries until this generation's previous decoder exits.
        long generation = decoder.cancel(); Uri uri = data.getData();
        selectedSource = uri; resumeSelected = false;
        pendingStart.offer(uri, generation);
        view.requestRender();
    }
    private long stopDecoder() {
        long generation = decoder.cancel();
        if (!decoder.awaitStopped(500)) report("旧解码器尚未退出或释放未确认");
        return generation;
    }
    private void reportStartFailure() {
        try { report("无法启动视频解码；请重新选择视频"); }
        catch (RuntimeException | OutOfMemoryError ignored) { /* best effort under allocation failure */ }
    }
    private void releaseUnstartedSurface(Surface surface, SurfaceTexture texture) {
        try { surface.release(); }
        catch (RuntimeException e) {
            // No decoder accepted this Surface, but failed release is still an
            // unknown local ownership state. Do not lose either object.
            decoder.retainUnstartedSurface(surface, texture);
            reportStartFailure();
        }
        catch (Error e) {
            decoder.retainUnstartedSurface(surface, texture);
            throw e;
        }
    }
    private void startDecoder(Uri uri, SurfaceTexture texture, long generation) {
        if (decoder.releaseUnconfirmed()) { reportStartFailure(); return; }
        final Surface surface;
        try { surface = new Surface(texture); }
        catch (RuntimeException | OutOfMemoryError | LinkageError e) { reportStartFailure(); return; }
        boolean started;
        try {
            DecoderResources<MediaCodec, MediaExtractor, Surface, SurfaceTexture> resources = new DecoderResources<>(texture, surface);
            started = decoder.start(generation, resources, token -> {
            boolean released;
            try {
                MediaExtractor extractor = new MediaExtractor(); resources.extractor = extractor;
                extractor.setDataSource(this, uri, null); int track = -1;
                for (int i = 0; i < extractor.getTrackCount(); i++)
                    if (extractor.getTrackFormat(i).getString(MediaFormat.KEY_MIME).startsWith("video/")) { track = i; break; }
                if (track < 0) throw new IllegalArgumentException("没有视频轨道");
                extractor.selectTrack(track); MediaFormat format = extractor.getTrackFormat(track); renderer.format(format);
                MediaCodec codec = MediaCodec.createDecoderByType(format.getString(MediaFormat.KEY_MIME)); resources.codec = codec;
                codec.configure(format, surface, null, 0); codec.start();
                MediaCodec.BufferInfo info = new MediaCodec.BufferInfo(); boolean inputEnded = false;
                long originNs = 0, originPts = 0;
                while (!token.get()) {
                    if (!inputEnded) { int input = codec.dequeueInputBuffer(10000);
                        if (input >= 0) { ByteBuffer bytes = codec.getInputBuffer(input); int n = extractor.readSampleData(bytes, 0);
                            if (n < 0) { codec.queueInputBuffer(input, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM); inputEnded = true; }
                            else { codec.queueInputBuffer(input, 0, n, extractor.getSampleTime(), 0); extractor.advance(); }
                        }
                    }
                    int output = codec.dequeueOutputBuffer(info, 10000);
                    if (output == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) renderer.format(codec.getOutputFormat());
                    else if (output >= 0) {
                        boolean eos = (info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0;
                        if (originNs == 0) { originNs = System.nanoTime(); originPts = info.presentationTimeUs; }
                        long due = originNs + (info.presentationTimeUs - originPts) * 1000;
                        codec.releaseOutputBuffer(output, Math.max(System.nanoTime(), due));
                        if (eos) break;
                    }
                }
            } catch (Exception e) { report("解码停止：" + e.getClass().getSimpleName()); }
            finally {
                released = resources.release(MediaCodec::stop, MediaCodec::release, MediaExtractor::release, Surface::release);
                if (!released) report("解码器释放未确认；不能切换视频");
            }
            return released;
            });
        } catch (RuntimeException | OutOfMemoryError | LinkageError e) {
            releaseUnstartedSurface(surface, texture);
            reportStartFailure();
            return;
        } catch (Error e) {
            releaseUnstartedSurface(surface, texture);
            throw e;
        }
        if (!started) releaseUnstartedSurface(surface, texture);
    }
    @Override protected void onPause() { resumed=false; resumeSelected=selectedSource!=null; pendingStart.clear(); long generation=stopDecoder(); if(decoder.ready(generation))view.queueEvent(() -> { if(decoder.ready(generation))renderer.reset(); }); view.onPause(); super.onPause(); }
    @Override protected void onResume() {
        super.onResume(); resumed=true;
        // onActivityResult may already have queued a newer selection while
        // paused. Otherwise restart the last source after the old producer.
        if (resumeSelected && selectedSource!=null && pendingStart.peek()==null) {
            long generation=decoder.cancel();pendingStart.offer(selectedSource,generation);
        }
        resumeSelected=false;view.onResume();view.requestRender();
    }
    @Override protected void onDestroy() { resumed=false; pendingStart.clear(); stopDecoder(); super.onDestroy(); }

    private void retryPendingStart() {
        startRetry.schedule(view::postDelayed, () -> resumed && pendingStart.peek() != null, view::requestRender);
    }

    private final class Renderer implements GLSurfaceView.Renderer, SurfaceTexture.OnFrameAvailableListener {
        SurfaceTexture texture; int oes, program, surfaceWidth, surfaceHeight;
        final Object textureLock = new Object();
        final class VideoFormat {final int width,height,primaries;final boolean knownSdr;VideoFormat(MediaFormat f){
            int w=f.getInteger(MediaFormat.KEY_WIDTH),h=f.getInteger(MediaFormat.KEY_HEIGHT);
            if(f.containsKey("crop-left")&&f.containsKey("crop-right"))w=f.getInteger("crop-right")-f.getInteger("crop-left")+1;
            if(f.containsKey("crop-top")&&f.containsKey("crop-bottom"))h=f.getInteger("crop-bottom")-f.getInteger("crop-top")+1;
            width=w;height=h;primaries=f.containsKey(MediaFormat.KEY_COLOR_STANDARD)?f.getInteger(MediaFormat.KEY_COLOR_STANDARD):0;
            knownSdr=primaries!=0&&f.containsKey(MediaFormat.KEY_COLOR_TRANSFER)&&f.getInteger(MediaFormat.KEY_COLOR_TRANSFER)==MediaFormat.COLOR_TRANSFER_SDR_VIDEO;
        }}
        volatile VideoFormat videoFormat; int configuredWidth,configuredHeight,configuredPrimaries;
        final AtomicBoolean available = new AtomicBoolean(); final float[] transform = new float[16];
        final ArrayList<AndroidMediaSession> retiring = new ArrayList<>();
        AndroidMediaSession session; AndroidMediaSession.Frame previous;
        EGLContext sessionContext; EGLDisplay sessionDisplay; Thread sessionThread;
        boolean contextWarning;
        final CompletionState completionState = new CompletionState();
        AndroidMediaSession.Completed lostCompletion;
        long sourceId; boolean unsupported;
        void format(MediaFormat f) {
            videoFormat=new VideoFormat(f);
        }
        @Override public void onSurfaceCreated(GL10 gl, EGLConfig config) {
            try { MediaRuntime.loadPackaged(); } catch (LinkageError e) { report("Native 不可用：" + e.getClass().getSimpleName()); unsupported = true; }
            int[] names = new int[1]; GLES30.glGenTextures(1, names, 0); oes = names[0];
            GLES30.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, oes);
            GLES30.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES30.GL_TEXTURE_MIN_FILTER, GLES30.GL_LINEAR);
            GLES30.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES30.GL_TEXTURE_MAG_FILTER, GLES30.GL_LINEAR);
            SurfaceTexture next = new SurfaceTexture(oes); next.setOnFrameAvailableListener(this);
            synchronized (textureLock) { texture = next; available.set(false); }
            String vertex = "#version 300 es\nprecision highp float;out vec2 uv;void main(){vec2 p=vec2(float((gl_VertexID<<1)&2),float(gl_VertexID&2));uv=p;gl_Position=vec4(p*2.-1.,0,1);}";
            String fragment = "#version 300 es\n#extension GL_OES_EGL_image_external_essl3 : require\nprecision highp float;in vec2 uv;uniform samplerExternalOES tex;uniform mat4 transform;out vec4 color;void main(){color=texture(tex,(transform*vec4(uv,0,1)).xy);}";
            int v=shader(GLES30.GL_VERTEX_SHADER,vertex), f=shader(GLES30.GL_FRAGMENT_SHADER,fragment);
            program=GLES30.glCreateProgram(); GLES30.glAttachShader(program,v); GLES30.glAttachShader(program,f); GLES30.glLinkProgram(program); GLES30.glDeleteShader(v); GLES30.glDeleteShader(f);
        }
        private int shader(int type,String text) { int id=GLES30.glCreateShader(type); GLES30.glShaderSource(id,text); GLES30.glCompileShader(id); return id; }
        @Override public void onSurfaceChanged(GL10 gl,int w,int h) { surfaceWidth=w;surfaceHeight=h; }
        @Override public void onFrameAvailable(SurfaceTexture source) {
            synchronized (textureLock) { if (source != texture) return; available.set(true); }
            view.requestRender();
        }
        private boolean compatibleContext() {
            EGLContext currentContext = EGL14.eglGetCurrentContext();
            EGLDisplay currentDisplay = EGL14.eglGetCurrentDisplay();
            if (EGL14.EGL_NO_CONTEXT.equals(currentContext) || EGL14.EGL_NO_DISPLAY.equals(currentDisplay)) return false;
            return sessionContext == null || (sessionThread == Thread.currentThread()
                && sessionContext.equals(currentContext) && sessionDisplay.equals(currentDisplay));
        }
        private void reportContextBlocked() {
            if (!contextWarning) { contextWarning = true; report("旧媒体会话的 EGL 上下文不可用，资源保留，无法安全切换"); }
        }
        boolean reset() {
            // A failed device has no proven retirement/recovery contract. Keep
            // the old session and its frames while allowing a new OES source.
            if (completionState.deviceLost()) { available.set(false); return true; }
            if (!compatibleContext()) { reportContextBlocked(); return false; }
            if(previous!=null){previous.close();previous=null;}
            if(session!=null){session.beginEpoch(session.epoch()+1);retiring.add(session);session=null;}
            completionState.newEpoch();
            retire(); available.set(false); unsupported=false;
            return true;
        }
        private void retire() {
            for(int i=retiring.size()-1;i>=0;i--) {
                AndroidMediaSession s=retiring.get(i);int status=s.poll(0);
                // Identity sessions in this sample all use ncnn's default
                // device 0. Do not treat distinct domain IDs as distinct GPUs.
                if(status==CompletionState.DEVICE_LOST){deviceLost();return;}
                if(status!=0){if(status!=-7&&status!=-12)report("资源退役等待："+status);continue;}
                try{s.close();retiring.remove(i);}catch(AndroidMediaSession.MediaException e){
                    if(e.status==CompletionState.DEVICE_LOST){deviceLost();return;}
                    if(e.status!=-7&&e.status!=-12)report("资源退役等待："+e.status);
                }
            }
            if(session==null&&retiring.isEmpty()){sessionContext=null;sessionDisplay=null;sessionThread=null;contextWarning=false;}
            if(!retiring.isEmpty())retirementRetry.schedule(view::postDelayed, () -> resumed, view::requestRender);
        }
        private void fallback() {
            GLES30.glBindFramebuffer(GLES30.GL_FRAMEBUFFER,0);GLES30.glViewport(0,0,surfaceWidth,surfaceHeight);
            GLES30.glUseProgram(program);GLES30.glActiveTexture(GLES30.GL_TEXTURE0);GLES30.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES,oes);
            GLES30.glUniformMatrix4fv(GLES30.glGetUniformLocation(program,"transform"),1,false,transform,0);GLES30.glDrawArrays(GLES30.GL_TRIANGLES,0,3);
        }
        private void deviceLost() {
            if (!completionState.deviceLost()) {
                completionState.failed(CompletionState.DEVICE_LOST);
                report("计算设备已丢失，保留原始视频；旧会话资源等待安全回收");
            }
        }
        private void retryCompletions() {
            if (completionState.needsPoll()) completionRetry.schedule(view::postDelayed, () -> resumed, view::requestRender);
        }
        private boolean checkProbe(long[] probe) {
            if (probe[0] == CompletionState.DEVICE_LOST) { deviceLost(); return false; }
            if (probe[0] != 0) throw new IllegalStateException("probe " + probe[0]);
            return true;
        }
        private boolean pollCompletions() {
            int poll = session.poll(0);
            if (poll == CompletionState.DEVICE_LOST) { deviceLost(); return false; }
            if (poll != 0 && poll != -7 && poll != -12) throw new IllegalStateException("poll " + poll);
            boolean drawn = false;
            for (;;) {
                AndroidMediaSession.Completed completed = session.pollCompleted();
                if (completed == null) break;
                if (completed.status == CompletionState.DEVICE_LOST) {
                    completionState.completed(completed.jobId, completed.status);
                    // Current JNI never publishes an error frame. Retain one if
                    // that contract is violated instead of releasing it here.
                    lostCompletion = completed;
                    report("计算设备已丢失，保留原始视频；旧会话资源等待安全回收");
                    return drawn;
                }
                if (completed.jobId == 0) {
                    if (completed.status != -8) throw new IllegalStateException("completion " + completed.status);
                    break;
                }
                completionState.completed(completed.jobId, completed.status);
                if (completed.frame != null) {
                    try {
                        if (completed.status == 0 && pendingStart.peek() == null) { session.draw(completed.frame,surfaceWidth,surfaceHeight); drawn = true; }
                    } catch (AndroidMediaSession.MediaException e) {
                        if (e.status == CompletionState.DEVICE_LOST) { lostCompletion = completed; deviceLost(); return drawn; }
                        throw e;
                    } finally { if (!completionState.deviceLost()) completed.frame.close(); }
                }
            }
            retryCompletions();
            return drawn;
        }
        private boolean processPendingStart() {
            DeferredStart.Request<Uri> request = pendingStart.peek();
            if (request == null) return false;
            if (!decoder.current(request.generation)) { pendingStart.discard(request); return true; }
            if (decoder.releaseUnconfirmed()) {
                if (pendingStart.discard(request)) report("解码器释放未确认；不能切换视频");
                return true;
            }
            if (!decoder.ready(request.generation) || texture == null) { retryPendingStart(); return true; }
            request = pendingStart.takeIfReady(true, decoder);
            if (request == null) return true;
            if (!reset()) return true;
            startDecoder(request.source, texture, request.generation);
            return true;
        }
        @Override public void onDrawFrame(GL10 gl) {
            if (!completionState.deviceLost() && !compatibleContext()) { reportContextBlocked(); return; }
            if (!completionState.deviceLost()) retire();
            boolean completedDrawn = false;
            if (session != null && !unsupported && !completionState.deviceLost()) {
                try { completedDrawn = pollCompletions(); }
                catch (RuntimeException e) {
                    if (e instanceof AndroidMediaSession.MediaException && ((AndroidMediaSession.MediaException)e).status == CompletionState.DEVICE_LOST) deviceLost();
                    else { unsupported=true; report("保留 OES 显示："+e.getMessage()); }
                }
            }
            if (processPendingStart()) return;
            if(!available.getAndSet(false)){if(completionState.deviceLost())fallback();return;}
            texture.updateTexImage(); texture.getTransformMatrix(transform);
            VideoFormat format=videoFormat;
            if(!completionState.deviceLost()&&session!=null&&(format==null||!format.knownSdr||format.width!=configuredWidth||format.height!=configuredHeight||format.primaries!=configuredPrimaries))reset();
            if(completionState.deviceLost()||unsupported||format==null||!format.knownSdr||format.width<=0||format.height<=0){fallback();return;}
            try {
                if(session==null){configuredWidth=format.width;configuredHeight=format.height;configuredPrimaries=format.primaries;session=AndroidMediaSession.identity(format.width,format.height,4,AndroidMediaSession.AHB_HOST_WAIT,null);
                    sessionContext=EGL14.eglGetCurrentContext();sessionDisplay=EGL14.eglGetCurrentDisplay();sessionThread=Thread.currentThread();
                    if(!checkProbe(session.probe())){fallback();return;}report("MediaCodec → OES → AHB → GLES（Identity；设备资格未认定）");}
                AndroidMediaSession.Frame current=session.capture(oes,transform,++sourceId,texture.getTimestamp()/1000,format.primaries,format.primaries);
                if(previous!=null){try{session.submit(previous,current,0.5,System.nanoTime()+80000000);completionState.accepted();retryCompletions();}catch(AndroidMediaSession.MediaException e){if(e.status!=-8&&e.status!=-9)throw e;}previous.close();}
                previous=current;
                if(!completedDrawn)fallback();
            } catch(RuntimeException e){if(e instanceof AndroidMediaSession.MediaException&&((AndroidMediaSession.MediaException)e).status==CompletionState.DEVICE_LOST)deviceLost();else{unsupported=true;report("保留 OES 显示："+e.getMessage());}fallback();}
        }
    }
}
