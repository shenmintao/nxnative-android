package org.nxnative;

import android.app.AlertDialog;
import android.app.NativeActivity;
import android.content.pm.PackageManager;
import android.hardware.display.DisplayManager;
import android.os.Handler;
import android.os.Looper;
import android.os.Bundle;
import android.os.Build;
import android.view.Display;
import android.view.View;
import android.view.WindowInsets;
import android.view.DisplayCutout;
import android.text.InputFilter;
import android.view.KeyEvent;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodManager;
import android.widget.EditText;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.atomic.AtomicReference;

/** Native game surface plus the device's normal, Unicode-capable screen keyboard. */
public final class GameNativeActivity extends NativeActivity {
    private native void setOverlayInsets(int left, int top, int right, int bottom, int width, int height, int dpi);
    private native void setDisplayRotation(int rotation);
    // A turn between landscape and reverse landscape (sensorLandscape) recreates nothing and resumes nothing: only the display changes.
    // The device-motion axes follow Display.getRotation(), so the native side hears of every change (V2405A, 2026-11-04: the gyro stayed mirrored)
    private DisplayManager.DisplayListener displayListener;
    private void reportRotation() {
        Display display = Build.VERSION.SDK_INT >= 30 ? getDisplay() : getWindowManager().getDefaultDisplay();
        if (display != null) setDisplayRotation(display.getRotation());
    }

    @Override public void onCreate(Bundle saved) {
        // A native method resolves only in a library loaded through System.loadLibrary: NativeActivity's own load in super.onCreate does not
        // register it with JNI (V2405A, 2026-11-04: UnsatisfiedLinkError for setOverlayInsets at the first insets dispatch). An external-data APK
        // starts here; with embedded data GameActivity has loaded it already, and loading it again is a no-op.
        String library = "dmhost";
        try { library = getPackageManager().getActivityInfo(getComponentName(), PackageManager.GET_META_DATA).metaData.getString("android.app.lib_name", library); }
        catch (PackageManager.NameNotFoundException ignored) { }
        System.loadLibrary(library);
        super.onCreate(saved);
        View decor = getWindow().getDecorView();
        decor.setOnApplyWindowInsetsListener((view, insets) -> {
            int left, top, right, bottom;
            if (Build.VERSION.SDK_INT >= 30) {
                android.graphics.Insets safe = insets.getInsets(WindowInsets.Type.systemBars()
                    | WindowInsets.Type.displayCutout() | WindowInsets.Type.mandatorySystemGestures());
                left = safe.left; top = safe.top; right = safe.right; bottom = safe.bottom;
            } else {
                left = insets.getSystemWindowInsetLeft(); top = insets.getSystemWindowInsetTop();
                right = insets.getSystemWindowInsetRight(); bottom = insets.getSystemWindowInsetBottom();
                if (Build.VERSION.SDK_INT >= 28) {
                    DisplayCutout cutout = insets.getDisplayCutout();
                    if (cutout != null) {
                        left = Math.max(left, cutout.getSafeInsetLeft()); top = Math.max(top, cutout.getSafeInsetTop());
                        right = Math.max(right, cutout.getSafeInsetRight()); bottom = Math.max(bottom, cutout.getSafeInsetBottom());
                    }
                }
                if (Build.VERSION.SDK_INT >= 29) {
                    android.graphics.Insets gestures = insets.getMandatorySystemGestureInsets();
                    left = Math.max(left, gestures.left); top = Math.max(top, gestures.top);
                    right = Math.max(right, gestures.right); bottom = Math.max(bottom, gestures.bottom);
                }
            }
            setOverlayInsets(left, top, right, bottom, view.getWidth(), view.getHeight(), getResources().getDisplayMetrics().densityDpi);
            return insets; // preserve normal NativeActivity/content/IME inset dispatch
        });
        decor.addOnLayoutChangeListener((v, l, t, r, b, ol, ot, or, ob) -> {
            if (r - l != or - ol || b - t != ob - ot) v.requestApplyInsets();
        });
        decor.post(() -> decor.requestApplyInsets());
        displayListener = new DisplayManager.DisplayListener() {
            @Override public void onDisplayAdded(int id) { }
            @Override public void onDisplayRemoved(int id) { }
            @Override public void onDisplayChanged(int id) { reportRotation(); }
        };
        ((DisplayManager) getSystemService(DISPLAY_SERVICE)).registerDisplayListener(displayListener, new Handler(Looper.getMainLooper()));
    }
    @Override protected void onDestroy() {
        if (displayListener != null) ((DisplayManager) getSystemService(DISPLAY_SERVICE)).unregisterDisplayListener(displayListener);
        displayListener = null;
        super.onDestroy();
    }
    // Called on the guest thread, which waits while the UI thread owns the dialog/IME.
    public byte[] requestText(byte[] title, byte[] initial, int maximum) {
        return requestText(title, initial, maximum, true);
    }
    public byte[] requestText(byte[] title, byte[] initial, int maximum, boolean xbox) {
        if (Looper.myLooper() == Looper.getMainLooper()) return null;
        CountDownLatch done = new CountDownLatch(1);
        AtomicReference<byte[]> result = new AtomicReference<>();
        new Handler(Looper.getMainLooper()).post(() -> {
            if (isFinishing() || isDestroyed()) { done.countDown(); return; }
            try {
                EditText edit = new EditText(this);
                edit.setSingleLine(true);
                edit.setImeOptions(EditorInfo.IME_ACTION_DONE);
                edit.setFilters(new InputFilter[] { new InputFilter.LengthFilter(Math.max(1, maximum)) });
                edit.setText(new String(initial, StandardCharsets.UTF_8));
                edit.setSelection(edit.length());
                int pad = (int) (24 * getResources().getDisplayMetrics().density);
                edit.setPadding(pad, pad, pad, pad);
                AlertDialog dialog = new AlertDialog.Builder(this)
                    .setTitle(new String(title, StandardCharsets.UTF_8)).setView(edit)
                    .setPositiveButton(android.R.string.ok, (d, which) -> result.set(edit.getText().toString().getBytes(StandardCharsets.UTF_8)))
                    .setNegativeButton(android.R.string.cancel, (d, which) -> {}).create();
                dialog.setOnDismissListener(d -> done.countDown());
                dialog.setOnKeyListener((d, key, event) -> {
                    if (event.getAction() != KeyEvent.ACTION_DOWN) return false;
                    if (key == (xbox ? KeyEvent.KEYCODE_BUTTON_A : KeyEvent.KEYCODE_BUTTON_B) || key == KeyEvent.KEYCODE_BUTTON_START) {
                        result.set(edit.getText().toString().getBytes(StandardCharsets.UTF_8)); dialog.dismiss(); return true;
                    }
                    if (key == (xbox ? KeyEvent.KEYCODE_BUTTON_B : KeyEvent.KEYCODE_BUTTON_A) || key == KeyEvent.KEYCODE_BUTTON_SELECT) { dialog.dismiss(); return true; }
                    return false;
                });
                edit.setOnEditorActionListener((v, action, event) -> {
                    if (action == EditorInfo.IME_ACTION_DONE) {
                        result.set(edit.getText().toString().getBytes(StandardCharsets.UTF_8)); dialog.dismiss(); return true;
                    }
                    return false;
                });
                dialog.setOnShowListener(d -> {
                    edit.requestFocus();
                    dialog.getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE);
                    ((InputMethodManager) getSystemService(INPUT_METHOD_SERVICE)).showSoftInput(edit, InputMethodManager.SHOW_IMPLICIT);
                });
                dialog.show();
            } catch (RuntimeException failure) { done.countDown(); }
        });
        try { done.await(); } catch (InterruptedException interrupted) { Thread.currentThread().interrupt(); return null; }
        return result.get();
    }
}
