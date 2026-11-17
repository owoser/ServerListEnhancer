package cn.owoser.sle;

import android.app.Activity;
import android.app.Application;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.PixelCopy;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Map;
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

    private HomeOverlay() {}
    private static native boolean nativeBindMainActivity(Class<?> mainActivityClass);
    public static native void nativeOpen();
    public static native boolean nativeTouch(float x, float y, int action);
    public static native boolean nativeBack();

    public static synchronized void install(String modRoot) throws Exception {
        final long epoch = ++generation;
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
            state.put("height", view == null ? 0 : view.surfaceHeight());
            snapshot = state.toString();
        } catch (Exception ignored) { snapshot = UNKNOWN; }
    }

    private static void tick() {
        boolean available = refreshEnvironment();
        boolean blocked = !available || isBlocked(activity);
        if (blocked && home) {
            closeActiveSession();
        }
        publishSnapshot(blocked);
    }

    private static void disposeView() {
        home = false;
        activeSession = 0L;
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
            PixelCopy.OnPixelCopyFinishedListener listener = result -> {
                        entryProbeInFlight = false;
                        boolean found = result == PixelCopy.SUCCESS && containsEntryMarker(shot);
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

    private static boolean containsEntryMarker(Bitmap shot) {
        int width = shot.getWidth();
        int height = shot.getHeight();
        int[] row = new int[width];
        for (int y = 0; y < height; ++y) {
            shot.getPixels(row, 0, width, 0, y, width, 1);
            for (int x = 0; x < width; ++x) {
                int pixel = row[x];
                int r = (pixel >> 16) & 255, g = (pixel >> 8) & 255, b = pixel & 255;
                if (r >= 200 && g <= 90 && b >= 200) return true;
            }
        }
        return false;
    }

    private static final class OverlayView extends View {
        int mode;
        long session;
        ArrayList<Command> commands = new ArrayList<>();
        final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        final Paint pixels = new Paint();
        final RectF destination = new RectF();
        final Path triangle = new Path();
        boolean listTracking;

        OverlayView(Activity owner) {
            super(owner);
            setWillNotDraw(false);
            pixels.setFilterBitmap(false);
            pixels.setAntiAlias(false);
            setFocusable(false);
            setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);
        }

        int surfaceWidth() { return getWidth() > 0 ? getWidth() : getRootView().getWidth(); }
        int surfaceHeight() { return getHeight() > 0 ? getHeight() : getRootView().getHeight(); }

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
            if (!focused) {
                closeActiveSession();
                publishSnapshot(true);
            }
        }

        @Override protected void onDetachedFromWindow() {
            super.onDetachedFromWindow();
            mode = 0;
            session = 0L;
            home = false;
            activeSession = 0L;
            snapshot = UNKNOWN;
        }

        void release() {
            commands.clear();
        }
    }
}
