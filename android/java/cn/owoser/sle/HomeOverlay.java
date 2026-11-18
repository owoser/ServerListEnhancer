package cn.owoser.sle;

import android.app.Activity;
import android.app.Application;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.net.Uri;
import android.util.Log;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.PixelCopy;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.widget.FrameLayout;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.net.Inet4Address;
import java.net.Inet6Address;
import java.net.InetAddress;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.SynchronousQueue;
import java.util.concurrent.ThreadFactory;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import org.json.JSONArray;
import org.json.JSONObject;

/** Mod-owned list overlay triggered by the native home button. UI mutations run on the main thread. */
public final class HomeOverlay {
    private static final String TAG = "SLE.HomeOverlay";
    private static final Handler UI = new Handler(Looper.getMainLooper());
    private static final String UNKNOWN = "{'ready':false,'home':false,'blocked':true,'session':0,'screen':'','width':0,'height':0}".replace((char) 39, (char) 34);
    private static final Map<String, Bitmap> IMAGES = new HashMap<>();
    private static volatile String snapshot = UNKNOWN;
    private static volatile boolean closed = true;
    private static volatile long generation;
    private static long sessionCounter;
    private static long activeSession;
    private static Application app;
    private static Field instanceField;
    private static Method resumedMethod;
    private static Method managerInstanceMethod;
    private static Method managerShowingMethod;
    private static Activity activity;
    private static OverlayView view;
    private static boolean home;
    private static boolean queryFailureLogged;
    private static Runnable ticker;
    private static long lastEntryProbeAt;
    private static boolean entryObservationLogged;
    private static boolean entryProbeInFlight;
    /** 连续失去窗口焦点超过该时长才确认列表应当关闭；截图、软键盘、系统弹窗只造成短时失焦。 */
    private static final long FOCUS_GRACE_MS = 1200L;
    /** 首次延迟确认的等待时间；仍在宽限期内时按剩余预算重复确认。 */
    private static final long FOCUS_CONFIRM_MS = 350L;
    private static long focusLostAt;
    private static Runnable focusConfirmation;

    private HomeOverlay() {}
    /** 加入请求的落地组件：LeviLauncher 自己的深链入口，会按游戏是否运行选择冷启动或热投递。 */
    private static final String JOIN_HANDLER_CLASS = "org.levimc.launcher.ui.activities.IntentHandler";
    /** 域名解析的最长等待；超时即放弃本次加入，不会发起任何 Intent。 */
    private static final long JOIN_RESOLVE_TIMEOUT_MS = 4000L;
    /** 解析线程池固定 2 线程、SynchronousQueue 直接交接（不排队），并发时拒绝而不是无限堆线程。 */
    private static final ThreadPoolExecutor JOIN_RESOLVER = new ThreadPoolExecutor(
            2, 2, 10L, TimeUnit.SECONDS, new SynchronousQueue<Runnable>(),
            new ThreadFactory() {
                @Override public Thread newThread(Runnable task) {
                    Thread thread = new Thread(task, "SLE-join-dns");
                    thread.setDaemon(true);
                    return thread;
                }
            },
            new ThreadPoolExecutor.AbortPolicy());
    private static final Object JOIN_LOCK = new Object();
    /** 每次加入请求的令牌：DNS 返回时令牌已过期就丢弃结果，避免关闭列表后仍然连服。 */
    private static long joinGeneration;
    private static boolean joinInFlight;
    /**
     * 本模组发起的加入会话：只有它会在"离开世界并停在 play_screen"时补一次 BACK 回首页。
     * 判据完全来自可观测状态：preloader 的 HUD/暂停菜单/菜单查询 + 资源包首页双标记 PixelCopy。
     */
    private static final long OWNED_JOIN_TIMEOUT_MS = 15L * 60L * 1000L;
    private static final long OWNED_JOIN_SAMPLE_MS = 450L;
    private static final long OWNED_JOIN_MENU_HOLD_MS = 900L;
    private static boolean ownedJoinArmed;
    private static long ownedJoinAt;
    private static boolean ownedJoinSeenHud;
    private static boolean ownedJoinSeenPause;
    private static long ownedJoinMenuSince;
    private static boolean ownedJoinNavDone;
    private static long ownedNavSampleAt;
    private static boolean homeMarkerProbeInFlight;
    private static long homeMarkerCheckedAt;
    private static volatile boolean homeMarkersPresent;
    private static boolean playMarkerProbeInFlight;
    private static long playMarkerCheckedAt;
    private static volatile boolean playMarkersPresent;
    private static Method hudScreenOpenMethod;
    private static Method pauseMenuOpenMethod;
    private static Method showingMenuMethod;
    private static boolean uiStateResolved;
    private static boolean uiStateUnavailable;

    private static native boolean nativeBindMainActivity(Class<?> mainActivityClass);
    public static native void nativeOpen();
    public static native boolean nativeTouch(float x, float y, int action);
    public static native boolean nativeBack();
    private static native void nativeJoinStatus(int status, String message);

    /**
     * 与列表绘制使用完全相同的字体配置测量文字宽度（ANTI_ALIAS_FLAG + Typeface.DEFAULT + max(1,size)）。
     * Paint 不是线程安全的，这里用 synchronized 串行化，可被任意线程调用；measureText 不受 Align 影响。
     */
    private static final Paint MEASURE_PAINT = new Paint(Paint.ANTI_ALIAS_FLAG);

    public static float measureText(String text, float size) {
        if (text == null) return -1f;
        synchronized (MEASURE_PAINT) {
            MEASURE_PAINT.setTypeface(Typeface.DEFAULT);
            MEASURE_PAINT.setTextSize(Math.max(1f, size));
            return MEASURE_PAINT.measureText(text);
        }
    }

    /**
     * 加入选中的服务器：地址是 IPv4 字面量时直接使用，域名则在后台线程解析（最多 4 秒）。
     * 结果回到主线程后才会构造 minecraft://connect 深链并启动 LevilLauncher 的 IntentHandler，
     * 因此 DNS 期间关闭列表、模块被禁用或界面销毁都不会留下迟到的连接。
     */
    public static void joinServer(final String address, final int port) {
        if (address == null || address.trim().isEmpty()) { reportJoin(-1, "服务器地址为空"); return; }
        if (port <= 0 || port > 65535) { reportJoin(-1, "服务器端口无效"); return; }
        final String host = address.trim();
        final long token;
        synchronized (JOIN_LOCK) {
            if (joinInFlight) { reportJoin(-2, "正在连接中，请稍候"); return; }
            joinInFlight = true;
            token = ++joinGeneration;
        }
        // 绑定发起时的界面身份：迟到的 DNS 结果只能在同一个覆盖层会话里落地。
        final long epochAtStart = generation;
        final long sessionAtStart = activeSession;
        final Activity activityAtStart = activity;
        String literal = literalIpv4(host);
        if (literal != null) {
            final String target = literal;
            UI.post(() -> launchJoin(token, target, port, epochAtStart, sessionAtStart, activityAtStart));
            return;
        }
        try {
            JOIN_RESOLVER.execute(() -> {
                String resolved = null;
                try { resolved = resolveJoinHost(host); } catch (Throwable ignored) { resolved = null; }
                final String target = resolved;
                UI.post(() -> {
                    if (target == null) { finishJoin(token, -4, "无法解析服务器地址"); return; }
                    launchJoin(token, target, port, epochAtStart, sessionAtStart, activityAtStart);
                });
            });
        } catch (RejectedExecutionException busy) {
            finishJoin(token, -6, "解析线程繁忙，请稍后重试");
            return;
        }
        UI.postDelayed(() -> expireJoin(token), JOIN_RESOLVE_TIMEOUT_MS);
    }

    /** 解析超时：令牌仍然有效时才回报失败，避免覆盖已经落地的结果。 */
    private static void expireJoin(long token) {
        synchronized (JOIN_LOCK) { if (!joinInFlight || token != joinGeneration) return; }
        finishJoin(token, -7, "解析地址超时，请检查网络后重试");
    }

    /** 取消尚未落地的加入请求：令牌失效后 DNS 结果与待执行的 startActivity 都会被丢弃。 */
    public static void cancelJoin() {
        synchronized (JOIN_LOCK) {
            joinInFlight = false;
            ++joinGeneration;
        }
    }

    private static void launchJoin(long token, String host, int port,
                                   long epochAtStart, long sessionAtStart, Activity activityAtStart) {
        if (!isJoinCurrent(token)) return;
        // 覆盖层已关闭、被重建、换过会话或换了 Activity 时一律丢弃，避免迟到结果在别的界面发起连接。
        if (closed || epochAtStart != generation || sessionAtStart != activeSession
                || activity == null || activity != activityAtStart) {
            finishJoin(token, -8, "界面已切换，已取消加入");
            return;
        }
        Activity current = activity;
        if (current.isFinishing() || current.isDestroyed()) {
            finishJoin(token, -3, "界面已关闭，已取消加入");
            return;
        }
        try {
            Uri uri = new Uri.Builder()
                    .scheme("minecraft")
                    .authority("connect")
                    .appendQueryParameter("serverUrl", host)
                    .appendQueryParameter("serverPort", Integer.toString(port))
                    .build();
            Intent intent = new Intent(Intent.ACTION_VIEW);
            intent.setClassName(current, JOIN_HANDLER_CLASS);
            intent.setData(uri);
            current.startActivity(intent);
            Log.i(TAG, "Join request dispatched: " + host + ":" + port);
            // 先让宿主把 joining 清掉（nativeJoinStatus 同步回调），再收起覆盖层，
            // 保证 C++ 侧不会把这次收尾当成用户取消而后又发一次 cancelJoin。
            finishJoin(token, 1, "正在加入 " + host + ":" + port);
            armOwnedJoinNavigation();
            closeActiveSession();
            publishSnapshot(isBlocked(current));
        } catch (Throwable failure) {
            Log.w(TAG, "joinServer failed", failure);
            finishJoin(token, -5, "无法发起加入请求");
        }
    }

    private static boolean isJoinCurrent(long token) {
        synchronized (JOIN_LOCK) { return joinInFlight && token == joinGeneration; }
    }

    private static void finishJoin(long token, int status, String message) {
        synchronized (JOIN_LOCK) {
            if (!joinInFlight || token != joinGeneration) return;
            joinInFlight = false;
            ++joinGeneration;
        }
        reportJoin(status, message);
    }

    private static void reportJoin(int status, String message) {
        try { nativeJoinStatus(status, message == null ? "" : message); }
        catch (Throwable failure) { Log.w(TAG, "nativeJoinStatus failed", failure); }
    }

    /** 已是 IPv4 字面量时原样返回（规范化点分十进制），否则返回 null 表示需要解析。 */
    private static String literalIpv4(String text) {
        String[] parts = text.split("\\.", -1);
        if (parts.length != 4) return null;
        StringBuilder normalized = new StringBuilder();
        for (String part : parts) {
            if (part.isEmpty() || part.length() > 3) return null;
            for (int i = 0; i < part.length(); ++i) {
                char c = part.charAt(i);
                if (c < '0' || c > '9') return null;
            }
            int value = Integer.parseInt(part);
            if (value > 255) return null;
            if (normalized.length() > 0) normalized.append('.');
            normalized.append(value);
        }
        return normalized.toString();
    }

    /** 域名解析优先 IPv4；没有 A 记录时才退回 IPv6。阻塞式，只在固定线程池里调用。 */
    private static String resolveJoinHost(String address) throws Exception {
        InetAddress[] addresses = InetAddress.getAllByName(address);
        String ipv4 = null, ipv6 = null;
        for (InetAddress candidate : addresses) {
            if (candidate instanceof Inet4Address) { if (ipv4 == null) ipv4 = candidate.getHostAddress(); }
            else if (candidate instanceof Inet6Address) { if (ipv6 == null) ipv6 = candidate.getHostAddress(); }
        }
        return ipv4 != null ? ipv4 : ipv6;
    }

    public static synchronized void install(String modRoot) throws Exception {
        final long epoch = ++generation;
        cancelJoin();
        closed = false;
        snapshot = UNKNOWN;
        resolveMethods(true);
        UI.post(() -> {
            if (closed || epoch != generation) return;
            disposeView();
            if (ticker != null) UI.removeCallbacks(ticker);
            home = false;
            activeSession = 0L;
            queryFailureLogged = false;
            ticker = new Runnable() {
                @Override public void run() {
                    if (closed || epoch != generation) return;
                    tick();
                    UI.postDelayed(this, 50L);
                }
            };
            ticker.run();
        });
    }

    public static String queryState() { return snapshot; }

    /** 当前被软键盘覆盖的高度（像素），0 表示键盘没有遮挡界面。 */
    public static int imeHeightPx() {
        OverlayView current = view;
        return current == null ? 0 : current.imeInset;
    }

    /** 去掉软键盘后可供界面排版的高度（像素）；queryState 的 height 已按该值上报。 */
    public static int availableHeightPx() {
        OverlayView current = view;
        return current == null ? 0 : Math.max(0, current.contentHeight());
    }

    /** 软键盘当前是否遮挡界面，原生侧可据此把输入区整体上移或压缩。 */
    public static boolean imeVisible() { return imeHeightPx() > 0; }

    public static void openFromHomeControl() {
        if (closed) return;
        final long epoch = generation;
        UI.post(() -> {
            if (closed || epoch != generation) return;
            if (!refreshEnvironment() || activity == null || activity.isFinishing() || activity.isDestroyed()) {
                return;
            }
            if (isBlocked(activity)) {
                closeActiveSession();
                publishSnapshot(false);
                return;
            }
            if (!ensureView()) {
                return;
            }
            activeSession = ++sessionCounter;
            home = true;
            view.mode = 2;
            view.session = activeSession;
            view.applyVisibility();
            publishSnapshot(false);
            try {
                nativeOpen();
            } catch (Throwable failure) {
                Log.w(TAG, "nativeOpen failed", failure);
                closeActiveSession();
                publishSnapshot(false);
            }
        });
    }

    public static void frame(String json) {
        if (closed || json == null) return;
        final long epoch = generation;
        int parsedMode = 0;
        long parsedSession = 0L;
        ArrayList<Command> parsedCommands = new ArrayList<>();
        try {
            JSONObject object = new JSONObject(json);
            parsedMode = object.optInt("mode", 0);
            if (parsedMode != 0 && parsedMode != 2) parsedMode = 0;
            parsedSession = object.optLong("session", 0L);
            JSONArray commands = object.optJSONArray("commands");
            if (parsedMode == 2 && commands != null && commands.length() <= 8192) {
                for (int i = 0; i < commands.length(); ++i) {
                    JSONObject command = commands.optJSONObject(i);
                    if (command != null) parsedCommands.add(new Command(command));
                }
            }
        } catch (Exception ignored) { parsedMode = 0; }
        final int mode = parsedMode;
        final long session = parsedSession;
        final ArrayList<Command> commands = parsedCommands;
        UI.post(() -> {
            if (closed || epoch != generation || view == null) return;
            if (mode == 0) {
                if (session != 0L && session != activeSession) return;
                closeActiveSession();
                publishSnapshot(isBlocked(activity));
                return;
            }
            if (!home || session == 0L || session != activeSession) return;
            view.mode = 2;
            view.session = session;
            view.commands = commands;
            view.applyVisibility();
        });
    }

    public static void image(String id, byte[] rgba, int width, int height) {
        if (closed || id == null || id.isEmpty() || rgba == null || width <= 0 || height <= 0
                || width > 4096 || height > 4096 || (long) width * height > 4000000L
                || rgba.length != (long) width * height * 4L) return;
        final long epoch = generation;
        final byte[] bytes = rgba.clone();
        UI.post(() -> {
            if (closed || epoch != generation) return;
            try {
                int[] pixels = new int[width * height];
                for (int i = 0, j = 0; i < pixels.length; ++i, j += 4) {
                    pixels[i] = ((bytes[j + 3] & 255) << 24) | ((bytes[j] & 255) << 16)
                            | ((bytes[j + 1] & 255) << 8) | (bytes[j + 2] & 255);
                }
                Bitmap bitmap = Bitmap.createBitmap(pixels, width, height, Bitmap.Config.ARGB_8888);
                Bitmap old = IMAGES.put(id, bitmap);
                if (old != null && old != bitmap && !old.isRecycled()) old.recycle();
                if (view != null && view.mode == 2) view.invalidate();
            } catch (RuntimeException failure) { Log.w(TAG, "Image update failed: " + id, failure); }
        });
    }

    public static synchronized void close() {
        // 模块卸载/界面销毁时作废未落地的加入请求，迟到的 DNS 结果不会再启动游戏。
        cancelJoin();
        closed = true;
        final long epoch = ++generation;
        snapshot = UNKNOWN;
        UI.post(() -> {
            if (epoch != generation) return;
            if (ticker != null) UI.removeCallbacks(ticker);
            ticker = null;
            home = false;
            activeSession = 0L;
            disposeView();
            releaseImages();
            app = null;
            instanceField = null;
            resumedMethod = null;
            managerInstanceMethod = null;
            managerShowingMethod = null;
        });
    }

    private static void resolveMethods(boolean bindNative) throws Exception {
        if (app == null) {
            Class<?> activityThread = Class.forName("android.app.ActivityThread");
            Method currentApplication = activityThread.getDeclaredMethod("currentApplication");
            currentApplication.setAccessible(true);
            app = (Application) currentApplication.invoke(null);
            if (app == null) throw new IllegalStateException("Application not ready");
        }
        if (instanceField == null || bindNative) {
            ClassLoader loader = app.getClassLoader();
            Class<?> main = Class.forName("com.mojang.minecraftpe.MainActivity", false, loader);
            if (bindNative && !nativeBindMainActivity(main)) {
                throw new IllegalStateException("Failed to bind MainActivity.launchUri");
            }
            Field foundInstance = main.getField("mInstance");
            Class<?> state = Class.forName("org.levimc.launcher.core.minecraft.MinecraftActivityState", false, loader);
            Method foundResumed = state.getMethod("isResumed");
            Class<?> manager = Class.forName("org.levimc.launcher.core.mods.inbuilt.overlay.InbuiltOverlayManager", false, loader);
            Method foundManager = manager.getMethod("getInstance");
            Method foundShowing = manager.getMethod("isModMenuShowing");
            resumedMethod = foundResumed;
            managerInstanceMethod = foundManager;
            managerShowingMethod = foundShowing;
            instanceField = foundInstance;
        }
    }

    private static boolean refreshEnvironment() {
        try {
            resolveMethods(false);
            Object current = instanceField.get(null);
            Activity next = current instanceof Activity ? (Activity) current : null;
            if (next != activity) {
                closeActiveSession();
                disposeView();
                activity = next;
            }
            if (activity != null && !activity.isFinishing() && !activity.isDestroyed()) {
                ensureView();
                return true;
            }
        } catch (Throwable failure) {
            if (!queryFailureLogged) {
                queryFailureLogged = true;
                Log.w(TAG, "Activity lookup unavailable", failure);
            }
        }
        return false;
    }

    private static boolean ensureView() {
        if (view != null) return true;
        if (activity == null) return false;
        View rootView = activity.findViewById(android.R.id.content);
        if (!(rootView instanceof ViewGroup)) return false;
        view = new OverlayView(activity);
        view.setVisibility(View.GONE);
        ((ViewGroup) rootView).addView(view, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        view.setZ(10000f);
        view.applyVisibility();
        return true;
    }

    private static boolean windowFocusHeld() {
        Activity current = activity;
        return current != null && !current.isFinishing() && !current.isDestroyed() && current.hasWindowFocus();
    }

    private static void clearFocusGrace() {
        focusLostAt = 0L;
        if (focusConfirmation != null) {
            UI.removeCallbacks(focusConfirmation);
            focusConfirmation = null;
        }
    }

    /** 失焦只记录起点并安排延迟确认：截图、软键盘、系统弹窗都会短暂夺走窗口焦点。 */
    private static void noteWindowFocus(boolean focused) {
        if (focused) {
            clearFocusGrace();
            return;
        }
        if (!home || activeSession == 0L) return;
        if (focusLostAt == 0L) focusLostAt = SystemClock.uptimeMillis();
        armFocusConfirmation();
    }

    private static void armFocusConfirmation() {
        if (focusConfirmation != null) return;
        focusConfirmation = new Runnable() {
            @Override public void run() {
                focusConfirmation = null;
                if (closed || !home || activeSession == 0L) {
                    focusLostAt = 0L;
                    return;
                }
                if (windowFocusHeld()) {
                    focusLostAt = 0L;
                    publishSnapshot(false);
                    return;
                }
                if (imeHeightPx() > 0) {
                    // 软键盘弹出期间的失焦不代表用户离开了列表。
                    focusLostAt = 0L;
                    return;
                }
                long now = SystemClock.uptimeMillis();
                if (focusLostAt == 0L) focusLostAt = now;
                if (now - focusLostAt < FOCUS_GRACE_MS) {
                    armFocusConfirmation();
                    return;
                }
                closeActiveSession();
                publishSnapshot(true);
            }
        };
        long delay = FOCUS_CONFIRM_MS;
        if (focusLostAt != 0L) {
            long elapsed = SystemClock.uptimeMillis() - focusLostAt;
            delay = Math.max(0L, Math.min(FOCUS_CONFIRM_MS, FOCUS_GRACE_MS - elapsed));
        }
        UI.postDelayed(focusConfirmation, delay);
    }

    /** tick 使用的阻塞判断：列表打开期间容忍短时失焦；列表关闭时保持原有立即阻塞判断。 */
    private static boolean blockedForSession(Activity currentActivity) {
        if (currentActivity == null || currentActivity.isFinishing() || currentActivity.isDestroyed()) return true;
        if (!home || activeSession == 0L) return isBlocked(currentActivity);
        try {
            boolean resumed = Boolean.TRUE.equals(resumedMethod.invoke(null));
            Object manager = managerInstanceMethod.invoke(null);
            boolean menuOpen = manager == null || Boolean.TRUE.equals(managerShowingMethod.invoke(manager));
            if (!resumed || menuOpen) return true;
            if (currentActivity.hasWindowFocus()) {
                clearFocusGrace();
                return false;
            }
            if (imeHeightPx() > 0) {
                clearFocusGrace();
                return false;
            }
            long now = SystemClock.uptimeMillis();
            if (focusLostAt == 0L) focusLostAt = now;
            armFocusConfirmation();
            return now - focusLostAt > FOCUS_GRACE_MS + 800L;
        } catch (Throwable failure) {
            return true;
        }
    }

    private static boolean isBlocked(Activity currentActivity) {
        if (currentActivity == null || currentActivity.isFinishing() || currentActivity.isDestroyed()) {
            return true;
        }
        try {
            boolean resumed = Boolean.TRUE.equals(resumedMethod.invoke(null));
            Object manager = managerInstanceMethod.invoke(null);
            // A missing manager is not proof that a module menu is closed.
            boolean menuOpen = manager == null || Boolean.TRUE.equals(managerShowingMethod.invoke(manager));
            return !resumed || !currentActivity.hasWindowFocus() || menuOpen;
        } catch (Throwable failure) {
            return true;
        }
    }

    private static void closeActiveSession() {
        home = false;
        activeSession = 0L;
        clearFocusGrace();
        if (view != null) {
            view.mode = 0;
            view.session = 0L;
            view.commands.clear();
            view.applyVisibility();
        }
    }

    private static void publishSnapshot(boolean blocked) {
        try {
            boolean ready = activity != null && !activity.isFinishing() && !activity.isDestroyed()
                    && view != null && view.isAttachedToWindow() && view.surfaceWidth() > 0 && view.surfaceHeight() > 0;
            boolean openNow = ready && home && !blocked && activeSession != 0L;
            JSONObject state = new JSONObject();
            state.put("ready", ready);
            state.put("home", openNow);
            state.put("blocked", blocked || !ready);
            state.put("session", openNow ? activeSession : 0L);
            state.put("screen", openNow ? "native_home_button" : "");
            state.put("width", view == null ? 0 : view.surfaceWidth());
            // 软键盘遮挡时上报可用高度，原生侧按该高度重新排版，底部按钮仍在键盘之上。
            state.put("height", view == null ? 0 : view.contentHeight());
            state.put("ime", view == null ? 0 : view.imeInset);
            snapshot = state.toString();
        } catch (Exception ignored) { snapshot = UNKNOWN; }
    }


    /** 解析 preloader 暴露的 UI 状态查询（Launcher 1.6.0 的 PreloaderInput 静态 native 方法）。 */
    private static void resolveUiStateProbe() {
        if (uiStateResolved || uiStateUnavailable) return;
        try {
            ClassLoader loader = app == null ? HomeOverlay.class.getClassLoader() : app.getClassLoader();
            Class<?> input = Class.forName("org.levimc.launcher.preloader.PreloaderInput", false, loader);
            hudScreenOpenMethod = input.getMethod("isHudScreenOpen");
            pauseMenuOpenMethod = input.getMethod("isPauseMenuOpen");
            showingMenuMethod = input.getMethod("isShowingMenu");
            uiStateResolved = true;
            Log.i(TAG, "Preloader UI state probe resolved");
        } catch (Throwable failure) {
            uiStateUnavailable = true;
            Log.w(TAG, "Preloader UI state probe unavailable", failure);
        }
    }

    /** 加入请求成功落地后开始跟踪这个会话：只有它退出世界时才会被导航回首页。 */
    private static void armOwnedJoinNavigation() {
        resolveUiStateProbe();
        ownedJoinArmed = true;
        ownedJoinAt = SystemClock.uptimeMillis();
        ownedJoinSeenHud = false;
        ownedJoinSeenPause = false;
        ownedJoinMenuSince = 0L;
        ownedJoinNavDone = false;
        ownedNavSampleAt = 0L;
        Log.i(TAG, "Owned join armed (return-to-home navigation)");
    }

    private static boolean uiState(Method method) {
        if (method == null) return false;
        try { return Boolean.TRUE.equals(method.invoke(null)); } catch (Throwable ignored) { return false; }
    }

    /**
     * 每 450 ms 采样一次 UI 状态：
     * 1) 必须先看到 HUD（证明真的进了世界）；
     * 2) HUD 与暂停菜单都关闭、isShowingMenu 为真、且首页双标记不可见 —— 说明停在某个非首页菜单（play_screen）；
     * 3) 该状态连续保持 900 ms 才补一次 BACK，每个加入会话最多一次，绝不重复或定时连按。
     */
    private static void pollOwnedJoinNavigation() {
        if (!ownedJoinArmed || ownedJoinNavDone) return;
        long now = SystemClock.uptimeMillis();
        if (now - ownedJoinAt > OWNED_JOIN_TIMEOUT_MS) {
            ownedJoinArmed = false;
            Log.i(TAG, "Owned join navigation expired without returning home");
            return;
        }
        if (now - ownedNavSampleAt < OWNED_JOIN_SAMPLE_MS) return;
        ownedNavSampleAt = now;
        boolean hud = uiState(hudScreenOpenMethod);
        boolean pause = uiState(pauseMenuOpenMethod);
        boolean menu = uiState(showingMenuMethod);
        if (hud && !ownedJoinSeenHud) {
            ownedJoinSeenHud = true;
            Log.i(TAG, "Owned join: HUD seen, session is in world");
        }
        if (pause && !ownedJoinSeenPause) {
            ownedJoinSeenPause = true;
            Log.i(TAG, "Owned join: pause menu seen");
        }
        maybeProbeHomeMarkers();
        maybeProbePlayMarkers();
        // 必须先在本次会话里真实见过 HUD（进入世界），否则退回首页这件事与我们无关。
        if (!ownedJoinSeenHud) return;
        // 只有资源包在 play_screen 上画出的双标记可见时才是"正向证明"，减法判据一律不触发导航。
        if (!playMarkersPresent) {
            ownedJoinMenuSince = 0L;
            return;
        }
        if (homeMarkersPresent) {
            ownedJoinMenuSince = 0L;
            Log.i(TAG, "Owned join: play markers visible but home markers also present, skip");
            return;
        }
        if (ownedJoinMenuSince == 0L) {
            ownedJoinMenuSince = now;
            Log.i(TAG, "Owned join: play_screen markers visible (hud=" + hud + " pause=" + pause + " menu=" + menu + ")");
        }
        if (now - ownedJoinMenuSince >= OWNED_JOIN_MENU_HOLD_MS) {
            ownedJoinNavDone = true;
            ownedJoinArmed = false;
            Log.i(TAG, "Owned join: sending one BACK to leave play_screen (positive marker)");
            pressGameBack();
        }
    }

    /** 通过 Activity 的按键分发注入一次 BACK：走游戏自己的 dispatchKeyEvent，绝不直接 finish Activity。 */
    private static void pressGameBack() {
        final Activity current = activity;
        if (current == null || current.isFinishing() || current.isDestroyed()) {
            Log.w(TAG, "Owned join: activity unavailable, BACK skipped");
            return;
        }
        UI.post(() -> {
            try {
                long now = SystemClock.uptimeMillis();
                current.dispatchKeyEvent(new KeyEvent(now, now, KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BACK, 0));
                current.dispatchKeyEvent(new KeyEvent(now, now, KeyEvent.ACTION_UP, KeyEvent.KEYCODE_BACK, 0));
            } catch (Throwable failure) {
                Log.w(TAG, "Owned join: BACK injection failed", failure);
            }
        });
    }

    /** 采样右上区域是否出现资源包画的首页双标记；只有 armed 会话才会用到。 */
    private static void maybeProbeHomeMarkers() {
        if (homeMarkerProbeInFlight || view == null) return;
        long now = SystemClock.uptimeMillis();
        if (now - homeMarkerCheckedAt < 1200L) return;
        homeMarkerCheckedAt = now;
        final Activity owner = activity;
        final OverlayView surface = view;
        if (owner == null || owner.isFinishing() || owner.isDestroyed() || surface == null) return;
        int width = surface.getWidth();
        int height = surface.getHeight();
        if (width <= 0 || height <= 0) return;
        int[] location = new int[2];
        surface.getLocationInWindow(location);
        final int left = location[0] + (int) (width * 0.5f);
        final int top = location[1];
        final int copyWidth = Math.max(1, location[0] + width - left);
        final int copyHeight = Math.max(1, (int) (height * 0.24f));
        final Bitmap shot = Bitmap.createBitmap(copyWidth, copyHeight, Bitmap.Config.ARGB_8888);
        homeMarkerProbeInFlight = true;
        final SurfaceView gameSurface = findSurfaceView(owner);
        PixelCopy.OnPixelCopyFinishedListener listener = result -> {
            homeMarkerProbeInFlight = false;
            boolean present = false;
            try { present = result == PixelCopy.SUCCESS && containsHomeMarkers(shot); }
            catch (Throwable ignored) { present = false; }
            shot.recycle();
            if (present != homeMarkersPresent) {
                homeMarkersPresent = present;
                Log.i(TAG, "Owned join: home markers " + (present ? "visible" : "hidden"));
            }
        };
        try {
            Rect source = new Rect(left, top, left + copyWidth, top + copyHeight);
            if (gameSurface != null) {
                int[] surfaceLocation = new int[2];
                gameSurface.getLocationInWindow(surfaceLocation);
                source.offset(-surfaceLocation[0], -surfaceLocation[1]);
                PixelCopy.request(gameSurface, source, shot, listener, UI);
            } else {
                source.offset(-location[0], -location[1]);
                PixelCopy.request(owner.getWindow(), source, shot, listener, UI);
            }
        } catch (Throwable failure) {
            homeMarkerProbeInFlight = false;
            shot.recycle();
        }
    }

    /** play_screen 的两个 1x1 标记：(198,195,195) 与 (195,195,198)，只在 play_screen 命名空间插入。 */
    private static final int PLAY_MARKER_SET = 195;
    private static final int PLAY_MARKER_HIGH = 198;

    private static boolean playMarkerLeftPixel(int pixel) {
        int red = (pixel >> 16) & 255, green = (pixel >> 8) & 255, blue = pixel & 255;
        return red == PLAY_MARKER_HIGH && green == PLAY_MARKER_SET && blue == PLAY_MARKER_SET;
    }

    private static boolean playMarkerRightPixel(int pixel) {
        int red = (pixel >> 16) & 255, green = (pixel >> 8) & 255, blue = pixel & 255;
        return red == PLAY_MARKER_SET && green == PLAY_MARKER_SET && blue == PLAY_MARKER_HIGH;
    }

    /** 左上角区域同时存在两个 play 标记（水平相隔 1~8 像素）才认为正处在 play_screen。 */
    private static boolean containsPlayMarkers(Bitmap shot) {
        int width = shot.getWidth();
        int height = shot.getHeight();
        if (width < 2 || height < 1) return false;
        int[] row = new int[width];
        for (int y = 0; y < height; ++y) {
            shot.getPixels(row, 0, width, 0, y, width, 1);
            for (int x = 0; x < width; ++x) {
                if (!playMarkerLeftPixel(row[x])) continue;
                for (int dx = 1; dx <= 8 && x + dx < width; ++dx) {
                    if (playMarkerRightPixel(row[x + dx])) return true;
                }
            }
        }
        return false;
    }

    /** 采样左上角是否出现 play_screen 标记；只有 armed 会话才会用到。 */
    private static void maybeProbePlayMarkers() {
        if (playMarkerProbeInFlight || view == null) return;
        long now = SystemClock.uptimeMillis();
        if (now - playMarkerCheckedAt < 700L) return;
        playMarkerCheckedAt = now;
        final Activity owner = activity;
        final OverlayView surface = view;
        if (owner == null || owner.isFinishing() || owner.isDestroyed() || surface == null) return;
        int[] location = new int[2];
        surface.getLocationInWindow(location);
        final int copyWidth = 160, copyHeight = 160;
        int screenWidth = surface.getWidth(), screenHeight = surface.getHeight();
        if (screenWidth <= 0 || screenHeight <= 0) return;
        final Bitmap shot = Bitmap.createBitmap(copyWidth, copyHeight, Bitmap.Config.ARGB_8888);
        playMarkerProbeInFlight = true;
        final SurfaceView gameSurface = findSurfaceView(owner);
        PixelCopy.OnPixelCopyFinishedListener listener = result -> {
            playMarkerProbeInFlight = false;
            boolean present = false;
            try { present = result == PixelCopy.SUCCESS && containsPlayMarkers(shot); }
            catch (Throwable ignored) { present = false; }
            shot.recycle();
            if (present != playMarkersPresent) {
                playMarkersPresent = present;
                Log.i(TAG, "Owned join: play_screen markers " + (present ? "visible" : "hidden"));
            }
        };
        try {
            Rect source = new Rect(location[0], location[1], location[0] + copyWidth, location[1] + copyHeight);
            if (gameSurface != null) {
                int[] surfaceLocation = new int[2];
                gameSurface.getLocationInWindow(surfaceLocation);
                source.offset(-surfaceLocation[0], -surfaceLocation[1]);
                PixelCopy.request(gameSurface, source, shot, listener, UI);
            } else {
                source.offset(-location[0], -location[1]);
                PixelCopy.request(owner.getWindow(), source, shot, listener, UI);
            }
        } catch (Throwable failure) {
            playMarkerProbeInFlight = false;
            shot.recycle();
        }
    }

    /** 右上区域同时存在 #C6C6C9（左标记）与 #C6C6C3（右标记）才认为仍在首页。 */
    private static boolean containsHomeMarkers(Bitmap shot) {
        int width = shot.getWidth();
        int height = shot.getHeight();
        if (width < 2 || height < 1) return false;
        int[] row = new int[width];
        boolean left = false, right = false;
        for (int y = 0; y < height && !(left && right); ++y) {
            shot.getPixels(row, 0, width, 0, y, width, 1);
            for (int x = 0; x < width; ++x) {
                int pixel = row[x];
                if (!left && entryMarkerLeftPixel(pixel)) left = true;
                else if (!right && entryMarkerPixel(pixel)) right = true;
                if (left && right) break;
            }
        }
        return left && right;
    }

    private static void tick() {
        boolean available = refreshEnvironment();
        if (view != null) view.pollIme();
        boolean blocked = !available || blockedForSession(activity);
        if (blocked && home) {
            closeActiveSession();
        }
        publishSnapshot(blocked);
        pollOwnedJoinNavigation();
    }

    private static void disposeView() {
        home = false;
        activeSession = 0L;
        clearFocusGrace();
        if (view != null) {
            view.mode = 0;
            view.session = 0L;
            view.setVisibility(View.GONE);
            view.release();
            if (view.getParent() instanceof ViewGroup) ((ViewGroup) view.getParent()).removeView(view);
            view = null;
        }
        activity = null;
    }

    private static void releaseImages() {
        for (Bitmap bitmap : IMAGES.values()) {
            if (bitmap != null && !bitmap.isRecycled()) bitmap.recycle();
        }
        IMAGES.clear();
    }

    private static final class Command {
        final int type, color;
        final float x, y, w, h, x3, y3, size;
        final String text, imageId;
        Command(JSONObject object) {
            type = object.optInt("type", -1);
            color = (int) object.optLong("color", 0L);
            x = finite(object.optDouble("x", 0)); y = finite(object.optDouble("y", 0));
            w = finite(object.optDouble("w", 0)); h = finite(object.optDouble("h", 0));
            x3 = finite(object.optDouble("x3", 0)); y3 = finite(object.optDouble("y3", 0));
            size = Math.max(0f, finite(object.optDouble("size", 0)));
            text = object.optString("text", ""); imageId = object.optString("imageId", "");
        }
        private static float finite(double value) { return Double.isNaN(value) || Double.isInfinite(value) ? 0f : (float) value; }
    }

    private static SurfaceView findSurfaceView(Activity owner) {
        try {
            View decor = owner.getWindow().getDecorView();
            return findSurfaceView(decor, 0);
        } catch (Throwable failure) {
            return null;
        }
    }

    private static SurfaceView findSurfaceView(View view, int depth) {
        if (view == null || depth > 12) return null;
        if (view instanceof SurfaceView) return (SurfaceView) view;
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            for (int i = group.getChildCount() - 1; i >= 0; --i) {
                SurfaceView found = findSurfaceView(group.getChildAt(i), depth + 1);
                if (found != null) return found;
            }
        }
        return null;
    }

    private static void probeHomeEntry(float x, float y) {
        if (view == null || entryProbeInFlight || closed) return;
        final View surface = view;
        int width = surface.getWidth();
        int height = surface.getHeight();
        if (width <= 0 || height <= 0) return;
        if (x < width * 0.5f || y > height * 0.22f) return;
        long now = SystemClock.uptimeMillis();
        if (now - lastEntryProbeAt < 600L) return;
        lastEntryProbeAt = now;
        final Activity owner = activity;
        if (owner == null || owner.isFinishing() || owner.isDestroyed()) return;
        int[] location = new int[2];
        surface.getLocationInWindow(location);
        final int left = location[0] + (int) (width * 0.5f);
        final int top = location[1];
        final int copyWidth = Math.max(1, location[0] + width - left);
        final int copyHeight = Math.max(1, (int) (height * 0.24f));
        final Bitmap shot = Bitmap.createBitmap(copyWidth, copyHeight, Bitmap.Config.ARGB_8888);
        entryProbeInFlight = true;
        final SurfaceView gameSurface = findSurfaceView(owner);
        try {
            final float localX = x - width * 0.5f;
            final float localY = y;
            PixelCopy.OnPixelCopyFinishedListener listener = result -> {
                        entryProbeInFlight = false;
                        boolean found = result == PixelCopy.SUCCESS && entryMarkerHit(shot, localX, localY);
                        shot.recycle();
                        if (found) {
                            openFromHomeControl();
                        } else if (result != PixelCopy.SUCCESS) {
                            Log.w(TAG, "home entry probe failed: " + result);
                        }
                    };
            Rect source = new Rect(left, top, left + copyWidth, top + copyHeight);
            if (gameSurface != null) {
                int[] surfaceLocation = new int[2];
                gameSurface.getLocationInWindow(surfaceLocation);
                source.offset(-surfaceLocation[0], -surfaceLocation[1]);
                PixelCopy.request(gameSurface, source, shot, listener, UI);
            } else {
                source.offset(-location[0], -location[1]);
                PixelCopy.request(owner.getWindow(), source, shot, listener, UI);
            }
        } catch (Throwable failure) {
            entryProbeInFlight = false;
            shot.recycle();
            Log.w(TAG, "PixelCopy unavailable", failure);
        }
    }

    /**
     * 首页标记色 #C6C6C3：r=g=198、b=195，与按钮灰 #C6C6C6 只差蓝通道 3，肉眼无法分辨；
     * 自然画面里几乎不会出现 r 与 g 相等、蓝通道又恰好低 2~4 的像素。
     */
    private static final int MARKER_RED_GREEN_TOLERANCE = 1;
    private static final int MARKER_BLUE_DROP_MIN = 2;
    private static final int MARKER_BLUE_DROP_MAX = 4;

    private static boolean entryMarkerPixel(int pixel) {
        int red = (pixel >> 16) & 255, green = (pixel >> 8) & 255, blue = pixel & 255;
        int redGreen = red > green ? red - green : green - red;
        int blueDrop = green - blue;
        return redGreen <= MARKER_RED_GREEN_TOLERANCE && blueDrop >= MARKER_BLUE_DROP_MIN && blueDrop <= MARKER_BLUE_DROP_MAX;
    }

    private static final int MARKER_BLUE_RISE_MIN = 2;
    private static final int MARKER_BLUE_RISE_MAX = 4;

    private static boolean entryMarkerLeftPixel(int pixel) {
        int red = (pixel >> 16) & 255, green = (pixel >> 8) & 255, blue = pixel & 255;
        int redGreen = red > green ? red - green : green - red;
        int blueRise = blue - green;
        return redGreen <= MARKER_RED_GREEN_TOLERANCE && blueRise >= MARKER_BLUE_RISE_MIN && blueRise <= MARKER_BLUE_RISE_MAX;
    }

    /**
     * 按钮左右各放一个 1 逻辑像素标记：左侧 #C6C6C9（蓝通道比绿通道高 3）、右侧 #C6C6C3（低 3）。
     * 只有触点横坐标严格落在两个标记之间、且纵向仍在同一按钮带内才算点中入口；
     * 因此点右侧的“社交”、或在其它页面点右上角都不会误开列表。
     */
    private static boolean entryMarkerHit(Bitmap shot, float touchX, float touchY) {
        int width = shot.getWidth();
        int height = shot.getHeight();
        if (width < 2 || height < 1) return false;
        int leftX = Integer.MAX_VALUE;
        int rightX = Integer.MIN_VALUE;
        int markerY = -1;
        int[] row = new int[width];
        for (int y = 0; y < height; ++y) {
            shot.getPixels(row, 0, width, 0, y, width, 1);
            for (int x = 0; x < width; ++x) {
                int pixel = row[x];
                if (entryMarkerPixel(pixel)) {
                    if (x > rightX) { rightX = x; markerY = y; }
                } else if (x < leftX && entryMarkerLeftPixel(pixel)) {
                    leftX = x;
                }
            }
        }
        if (rightX == Integer.MIN_VALUE || leftX == Integer.MAX_VALUE || leftX >= rightX) return false;
        if (touchX <= leftX || touchX >= rightX) return false;
        int band = Math.max(48, (int) (height * 0.35f));
        return markerY >= 0 && Math.abs(touchY - markerY) <= band;
    }

    /** API 30+ 才有精确的 IME inset；单独成类，旧系统不会解析这个类里缺失的类型。 */
    private static final class InsetsApi30 {
        static int imeBottom(WindowInsets insets) {
            if (insets == null) return 0;
            android.graphics.Insets ime = insets.getInsets(WindowInsets.Type.ime());
            return ime == null ? 0 : Math.max(0, ime.bottom);
        }
    }

    private static final class OverlayView extends View {
        int mode;
        long session;
        ArrayList<Command> commands = new ArrayList<>();
        final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        final Paint pixels = new Paint();
        final RectF destination = new RectF();
        final Path triangle = new Path();
        final Rect visibleFrame = new Rect();
        boolean listTracking;
        int imeInset;
        long imeMeasuredAt;
        int baselineWidth;
        int baselineHeight;
        boolean insetsReported;

        OverlayView(Activity owner) {
            super(owner);
            setWillNotDraw(false);
            pixels.setFilterBitmap(false);
            pixels.setAntiAlias(false);
            setFocusable(false);
            setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);
            setOnApplyWindowInsetsListener((target, insets) -> {
                applyWindowInsets(insets);
                return insets;
            });
        }

        int surfaceWidth() { return getWidth() > 0 ? getWidth() : getRootView().getWidth(); }
        int surfaceHeight() { return getHeight() > 0 ? getHeight() : getRootView().getHeight(); }

        /** 可供排版的高度：软键盘遮挡时扣掉 IME 高度；窗口已被系统压缩时不再重复扣减。 */
        int contentHeight() {
            int height = surfaceHeight();
            if (imeInset <= 0 || height <= 0) return height;
            if (baselineWidth == surfaceWidth() && baselineHeight > 0 && height + imeInset <= baselineHeight) return height;
            return Math.max(1, height - imeInset);
        }

        void applyWindowInsets(WindowInsets insets) {
            if (insets == null) return;
            if (Build.VERSION.SDK_INT >= 30) {
                insetsReported = true;
                applyImeInset(InsetsApi30.imeBottom(insets));
            } else {
                pollIme();
            }
        }

        /** API 28 兼容路径：用可见显示区域估算软键盘遮挡高度。 */
        void pollIme() {
            long now = SystemClock.uptimeMillis();
            if (now - imeMeasuredAt < 200L) return;
            imeMeasuredAt = now;
            if (Build.VERSION.SDK_INT >= 30 && insetsReported) return;
            if (!isAttachedToWindow()) return;
            View root = getRootView();
            int rootHeight = root == null ? 0 : root.getHeight();
            if (rootHeight <= 0) return;
            int[] rootLocation = new int[2];
            try {
                getWindowVisibleDisplayFrame(visibleFrame);
                root.getLocationOnScreen(rootLocation);
            } catch (Throwable failure) {
                return;
            }
            if (visibleFrame.bottom <= 0) return;
            int covered = rootLocation[1] + rootHeight - visibleFrame.bottom;
            // 导航栏远小于键盘：不足视口 12% 的遮挡不当作软键盘。
            int threshold = Math.max(1, rootHeight * 12 / 100);
            applyImeInset(covered > threshold ? covered : 0);
        }

        void applyImeInset(int raw) {
            int height = surfaceHeight();
            int inset = 0;
            if (raw > 0 && height > 0) {
                int[] location = new int[2];
                getLocationInWindow(location);
                View root = getRootView();
                int rootHeight = root == null ? 0 : root.getHeight();
                int bottomOffset = rootHeight > 0 ? Math.max(0, rootHeight - (location[1] + getHeight())) : 0;
                inset = Math.min(Math.max(0, raw - bottomOffset), height * 3 / 5);
            }
            if (inset == imeInset) return;
            imeInset = inset;
            invalidate();
            // 可用高度变化会让原生侧重新排版，立即刷新快照，避免最多 50ms 的旧高度。
            UI.post(() -> { if (!closed) tick(); });
        }

        void applyVisibility() {
            boolean visible = !closed && home && mode == 2 && session != 0L && session == activeSession
                    && surfaceWidth() > 0 && surfaceHeight() > 0;
            if (!visible) {
                listTracking = false;
                clearFocus();
            }
            setVisibility(closed || activity == null ? View.GONE : View.VISIBLE);

            if (visible) {
                setFocusableInTouchMode(true);
                if (!hasFocus()) requestFocus();
                invalidate();
            } else {
                setFocusableInTouchMode(false);
                setFocusable(false);
                clearFocus();
            }
        }

        @Override protected void onSizeChanged(int width, int height, int oldWidth, int oldHeight) {
            super.onSizeChanged(width, height, oldWidth, oldHeight);
            if (imeInset <= 0 && width > 0 && height > 0) {
                baselineWidth = width;
                baselineHeight = height;
            }
        }

        @Override protected void onDraw(Canvas canvas) {
            super.onDraw(canvas);
            if (closed || !home || mode != 2 || session != activeSession) return;
            if (commands.isEmpty()) {
                canvas.drawColor(0xFF1C201E);
                return;
            }
            for (Command command : commands) drawCommand(canvas, command);
        }

        private void drawCommand(Canvas canvas, Command command) {
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(command.color);
            paint.setTypeface(Typeface.DEFAULT);
            switch (command.type) {
                case 0:
                    paint.setTextSize(Math.max(1f, command.size));
                    if (command.w > 0f && command.h > 0f) {
                        paint.setTextAlign(Paint.Align.CENTER);
                        float textY = command.y + command.h / 2f - (paint.ascent() + paint.descent()) / 2f;
                        canvas.drawText(command.text, command.x + command.w / 2f, textY, paint);
                    } else {
                        paint.setTextAlign(command.w == -1f ? Paint.Align.RIGHT : command.w == -2f ? Paint.Align.CENTER : Paint.Align.LEFT);
                        canvas.drawText(command.text, command.x, command.y, paint);
                    }
                    paint.setTextAlign(Paint.Align.LEFT);
                    break;
                case 1:
                    paint.setStyle(Paint.Style.STROKE);
                    paint.setStrokeWidth(Math.max(1f, command.size));
                    canvas.drawRect(command.x, command.y, command.x + command.w, command.y + command.h, paint);
                    break;
                case 2:
                    paint.setStrokeWidth(Math.max(1f, command.size));
                    canvas.drawLine(command.x, command.y, command.x + command.w, command.y + command.h, paint);
                    break;
                case 3:
                    canvas.drawRect(command.x, command.y, command.x + command.w, command.y + command.h, paint);
                    break;
                case 4:
                    canvas.drawCircle(command.x, command.y, command.size, paint);
                    break;
                case 5:
                    triangle.reset();
                    triangle.moveTo(command.x, command.y);
                    triangle.lineTo(command.w, command.h);
                    triangle.lineTo(command.x3, command.y3);
                    triangle.close();
                    canvas.drawPath(triangle, paint);
                    break;
                case 6:
                    Bitmap bitmap = IMAGES.get(command.imageId);
                    if (bitmap != null && !bitmap.isRecycled()) {
                        destination.set(command.x, command.y, command.x + command.w, command.y + command.h);
                        canvas.drawBitmap(bitmap, null, destination, pixels);
                    }
                    break;
                default:
                    break;
            }
        }

        @Override public boolean onTouchEvent(MotionEvent event) {
            if (closed || activity == null) return false;
            int action = event.getActionMasked();
            if (home && mode == 2 && session == activeSession) {
                float x = event.getX(), y = event.getY();
                listTracking = action != MotionEvent.ACTION_UP && action != MotionEvent.ACTION_CANCEL;
                try { nativeTouch(x, y, action); }
                catch (Throwable failure) { closeActiveSession(); publishSnapshot(false); }
                return true;
            }
            if (action == MotionEvent.ACTION_DOWN) probeHomeEntry(event.getX(), event.getY());
            return false;
        }

        @Override public boolean dispatchKeyEvent(KeyEvent event) {
            if (home && mode == 2 && session == activeSession
                    && (event.getKeyCode() == KeyEvent.KEYCODE_BACK || event.getKeyCode() == KeyEvent.KEYCODE_ESCAPE)) {
                if (event.getAction() == KeyEvent.ACTION_UP) {
                    try { nativeBack(); }
                    catch (Throwable failure) { closeActiveSession(); publishSnapshot(false); }
                }
                return true;
            }
            return super.dispatchKeyEvent(event);
        }

        @Override public void onWindowFocusChanged(boolean focused) {
            super.onWindowFocusChanged(focused);
            noteWindowFocus(focused);
        }

        @Override protected void onAttachedToWindow() {
            super.onAttachedToWindow();
            insetsReported = false;
            requestApplyInsets();
        }

        @Override protected void onDetachedFromWindow() {
            super.onDetachedFromWindow();
            mode = 0;
            session = 0L;
            home = false;
            activeSession = 0L;
            imeInset = 0;
            insetsReported = false;
            snapshot = UNKNOWN;
        }

        void release() {
            commands.clear();
        }
    }
}
