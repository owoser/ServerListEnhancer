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
    private static boolean entryObservationLogged;
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

    /** 打开入口遇到瞬时 block（焦点/启动器菜单态）时的有界重试：6 次 × 120ms ≈ 700ms。 */
    private static final int HOME_OPEN_MAX_ATTEMPTS = 6;
    private static final long HOME_OPEN_RETRY_MS = 120L;

    /** 全屏像素宽度（原生触摸回调把按钮矩形换算成比例坐标用）；覆盖层尚未创建时回退到屏幕尺寸。 */
    public static int screenWidthPx() {
        OverlayView current = view;
        if (current != null) {
            int width = current.surfaceWidth();
            if (width > 0) return width;
        }
        Activity owner = activity;
        return owner == null ? 0 : owner.getResources().getDisplayMetrics().widthPixels;
    }

    /** 全屏像素高度；覆盖层尚未创建时回退到屏幕尺寸。 */
    public static int screenHeightPx() {
        OverlayView current = view;
        if (current != null) {
            int height = current.surfaceHeight();
            if (height > 0) return height;
        }
        Activity owner = activity;
        return owner == null ? 0 : owner.getResources().getDisplayMetrics().heightPixels;
    }

    private static long homeMenuCheckedAt;
    private static boolean homeMenuCached;
    private static java.lang.reflect.Method homeHudMethod;
    private static java.lang.reflect.Method homePauseMethod;
    private static java.lang.reflect.Method homeMenuMethod;

    /**
     * 当前是否停在某个菜单界面（不在世界内、不在暂停菜单），供原生触摸回调判断"首页"。
     * 400ms 缓存，避免每次触摸都反射。
     */
    public static boolean gameHomeMenu() {
        long now = SystemClock.uptimeMillis();
        if (now - homeMenuCheckedAt < 400L) return homeMenuCached;
        homeMenuCheckedAt = now;
        try {
            if (homeMenuMethod == null) {
                ClassLoader loader = app == null ? HomeOverlay.class.getClassLoader() : app.getClassLoader();
                Class<?> input = Class.forName("org.levimc.launcher.preloader.PreloaderInput", false, loader);
                homeHudMethod = input.getMethod("isHudScreenOpen");
                homePauseMethod = input.getMethod("isPauseMenuOpen");
                homeMenuMethod = input.getMethod("isShowingMenu");
            }
            boolean hud = Boolean.TRUE.equals(homeHudMethod.invoke(null));
            boolean pause = Boolean.TRUE.equals(homePauseMethod.invoke(null));
            boolean menu = Boolean.TRUE.equals(homeMenuMethod.invoke(null));
            // 首页与游戏列表都在 hud=false / pause=false 的菜单层；isShowingMenu 在部分界面为假，只作日志。
            homeMenuCached = !hud && !pause;
            Log.i(TAG, "preloader state hud=" + hud + " pause=" + pause + " menu=" + menu);
        } catch (Throwable ignored) {
            homeMenuCached = false;
        }
        return homeMenuCached;
    }

    public static void openFromHomeControl() {
        if (closed) return;
        tryOpenFromHome(generation, 1);
    }

    private static void tryOpenFromHome(final long epoch, final int attempt) {
        if (closed || epoch != generation) return;
        UI.post(() -> {
            if (closed || epoch != generation) return;
            if (!refreshEnvironment() || activity == null || activity.isFinishing() || activity.isDestroyed()) {
                return;
            }
            if (isBlocked(activity)) {
                boolean focus = activity.hasWindowFocus();
                String menuOpen = describeMenuOpen();
                if (attempt < HOME_OPEN_MAX_ATTEMPTS) {
                    Log.i(TAG, "open from home control retry " + attempt + " focus=" + focus + " menuOpen=" + menuOpen);
                    UI.postDelayed(() -> tryOpenFromHome(epoch, attempt + 1), HOME_OPEN_RETRY_MS);
                    return;
                }
                Log.i(TAG, "open from home control gave up after " + HOME_OPEN_MAX_ATTEMPTS + " tries focus=" + focus + " menuOpen=" + menuOpen);
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

    /** 诊断用：读取启动器菜单管理器当前是否认为菜单打开。 */
    private static String describeMenuOpen() {
        try {
            Object manager = managerInstanceMethod.invoke(null);
            if (manager == null) return "no-manager";
            return String.valueOf(Boolean.TRUE.equals(managerShowingMethod.invoke(manager)));
        } catch (Throwable failure) {
            return "err";
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
            // 视觉上再画约 180ms 淡出；逻辑上已关闭，期间不再接收触摸。
            if (view.beginDismissFade()) return;
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

    /** 加入请求成功落地后开始跟踪这个会话：只有它退出世界时才会被导航回首页。 */

    /** 通过 Activity 的按键分发注入一次 BACK：走游戏自己的 dispatchKeyEvent，绝不直接 finish Activity。 */

    /** 兜底：走 Activity 自己的返回处理，仅在上一次按键注入无效时使用一次。 */

    /** 采样右上区域是否出现资源包画的首页双标记；只有 armed 会话才会用到。 */

    /** 右上区域同时存在 #C6C6C9（左标记）与 #C6C6C3（右标记）才认为仍在首页。 */

    private static void tick() {
        boolean available = refreshEnvironment();
        if (view != null) view.pollIme();
        boolean blocked = !available || blockedForSession(activity);
        if (blocked && home) {
            closeActiveSession();
        }
        publishSnapshot(blocked);
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



    /**
     * 首页标记色 #C6C6C3：r=g=198、b=195，与按钮灰 #C6C6C6 只差蓝通道 3，肉眼无法分辨；
     * 自然画面里几乎不会出现 r 与 g 相等、蓝通道又恰好低 2~4 的像素。
     */





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
        // 整层淡入淡出：只改绘制透明度，不动布局、命中判据与加入逻辑。
        private static final long FADE_DURATION_MS = 180L;
        private float fadeAlpha = 1f;
        private float fadeFrom = 1f;
        private float fadeTo = 1f;
        private long fadeStartedAt;
        private boolean fading;
        private boolean dismissing;
        private boolean overlayActive;
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
            if (closed || activity == null) {
                overlayActive = false;
                setVisibility(View.GONE);
                return;
            }
            // 淡出期间节点仍可见，只是不再接收输入；透明度由 onDraw 逐帧推进。
            setVisibility(View.VISIBLE);

            if (visible) {
                if (!overlayActive) {
                    overlayActive = true;
                    fadeAlpha = 0f;
                    startFade(1f);
                } else if (dismissing) {
                    dismissing = false;
                    startFade(1f);
                } else if (!fading && fadeAlpha < 0.999f) {
                    startFade(1f);
                }
                setFocusableInTouchMode(true);
                if (!hasFocus()) requestFocus();
                invalidate();
            } else {
                setFocusableInTouchMode(false);
                setFocusable(false);
                clearFocus();
                if (!dismissing) overlayActive = false;
                invalidate();
            }
        }

        /** 淡入淡出共用：从当前 alpha 平滑推到目标值，按帧间隔推进而不是固定步进。 */
        private void startFade(float target) {
            fadeFrom = fadeAlpha;
            fadeTo = target;
            fadeStartedAt = SystemClock.uptimeMillis();
            fading = true;
        }

        /** 每帧推进透明度：smoothstep 曲线，与 Windows 侧 180ms 过渡保持一致。 */
        private void advanceFade() {
            if (!fading) return;
            long now = SystemClock.uptimeMillis();
            float t = (now - fadeStartedAt) / (float) FADE_DURATION_MS;
            if (t < 0f) t = 0f;
            if (t >= 1f) {
                fadeAlpha = fadeTo;
                fading = false;
                if (fadeAlpha <= 0.001f) {
                    finishDismiss();
                    return;
                }
                invalidate();
                return;
            }
            fadeAlpha = fadeFrom + (fadeTo - fadeFrom) * (t * t * (3f - 2f * t));
            postInvalidateOnAnimation();
        }

        /** 淡出结束才真正收起：清掉旧帧并让节点退到不可见。 */
        private void finishDismiss() {
            dismissing = false;
            fading = false;
            overlayActive = false;
            fadeAlpha = 1f;
            mode = 0;
            session = 0L;
            commands.clear();
            applyVisibility();
        }

        /** 关闭时启动淡出；没有可画的帧时返回 false，直接走原来的立即收起。 */
        boolean beginDismissFade() {
            if (closed || activity == null) return false;
            if (surfaceWidth() <= 0 || surfaceHeight() <= 0 || commands.isEmpty()) return false;
            if (dismissing) return true;
            dismissing = true;
            overlayActive = true;
            setFocusableInTouchMode(false);
            setFocusable(false);
            clearFocus();
            startFade(0f);
            invalidate();
            return true;
        }

        /** 把整层透明度乘进颜色 alpha：文字、矩形、线段、圆、三角形共用同一系数。 */
        private int fadeColor(int color) {
            if (fadeAlpha >= 0.999f) return color;
            int alpha = Math.round(((color >>> 24) & 0xFF) * fadeAlpha);
            return (alpha << 24) | (color & 0x00FFFFFF);
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
            // 先推进透明度：淡出期间即使不再有新帧，也按帧率继续重绘直到全透明。
            advanceFade();
            if (closed) return;
            if (!overlayActive && !(home && mode == 2 && session == activeSession)) return;
            if (commands.isEmpty()) {
                canvas.drawColor(fadeColor(0xFF1C201E));
                return;
            }
            for (Command command : commands) drawCommand(canvas, command);
        }

        private void drawCommand(Canvas canvas, Command command) {
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(fadeColor(command.color));
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
                        // 图片与文字/形状共用同一整层 alpha，不会有的元素先消失。
                        pixels.setAlpha(Math.round(255f * fadeAlpha));
                        canvas.drawBitmap(bitmap, null, destination, pixels);
                    }
                    break;
                default:
                    break;
            }
        }

        @Override public boolean onTouchEvent(MotionEvent event) {
            if (closed || activity == null || dismissing) return false;
            int action = event.getActionMasked();
            if (home && mode == 2 && session == activeSession) {
                float x = event.getX(), y = event.getY();
                listTracking = action != MotionEvent.ACTION_UP && action != MotionEvent.ACTION_CANCEL;
                try { nativeTouch(x, y, action); }
                catch (Throwable failure) { closeActiveSession(); publishSnapshot(false); }
                return true;
            }
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
