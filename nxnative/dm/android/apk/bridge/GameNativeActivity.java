package org.nxnative;

import android.app.AlertDialog;
import android.app.NativeActivity;
import android.os.Handler;
import android.os.Looper;
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
