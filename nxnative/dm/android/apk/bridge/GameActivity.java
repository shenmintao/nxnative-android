package org.nxnative;

import android.app.AlertDialog;
import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.res.AssetManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.WindowManager;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;
import java.util.Locale;
import java.io.File;

/** Shows preparation progress without touching the game's Vulkan surface. */
public final class GameActivity extends Activity {
    private final Handler ui = new Handler(Looper.getMainLooper());
    private AlertDialog dialog;
    private ProgressBar progress;
    private TextView detail;
    private boolean ready;
    private boolean resumed, prepared, launched;
    private native String prepareGameData(AssetManager assets, String root);

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        TextView waiting = new TextView(this);
        waiting.setText("Preparing game data...");
        waiting.setPadding(32, 32, 32, 32);
        setContentView(waiting);
        new Thread(() -> {
            String error = null;
            try {
                String library = getPackageManager().getActivityInfo(getComponentName(), PackageManager.GET_META_DATA).metaData.getString("android.app.lib_name", "dmhost");
                System.loadLibrary(library);
                File root = getExternalFilesDir(null);
                if (root == null) root = getFilesDir();
                onGameDataProgress(0, 0);
                error = prepareGameData(getAssets(), root.getAbsolutePath());
            } catch (Exception | UnsatisfiedLinkError failure) {
                error = "Cannot prepare game data: " + failure.getMessage();
            }
            onGameDataReady(error);
        }, "game-data").start();
    }

    @Override protected void onResume() { super.onResume(); resumed = true; launchIfReady(); }
    @Override protected void onPause() { resumed = false; super.onPause(); }
    private void launchIfReady() {
        if (!prepared || !resumed || launched || isFinishing()) return;
        launched = true;
        Intent intent = new Intent(this, GameNativeActivity.class);
        intent.addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        startActivity(intent);
        finish();
    }

    // Called on the native host thread; all view changes run on the UI thread.
    public void onGameDataProgress(long done, long total) {
        ui.post(() -> {
            if (ready || isFinishing() || isDestroyed()) return;
            if (dialog == null) {
                int pad = (int) (24 * getResources().getDisplayMetrics().density);
                LinearLayout layout = new LinearLayout(this);
                layout.setOrientation(LinearLayout.VERTICAL);
                layout.setPadding(pad, pad, pad, pad);
                progress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
                progress.setMax(1000);
                detail = new TextView(this);
                layout.addView(progress);
                layout.addView(detail);
                dialog = new AlertDialog.Builder(this)
                    .setTitle("Preparing game data")
                    .setMessage("The first launch needs space for the unpacked game. Interrupted preparation resumes on the next launch.")
                    .setView(layout).setCancelable(false).create();
                dialog.show();
            }
            progress.setIndeterminate(total == 0);
            if (total > 0) {
                progress.setProgress((int) (1000.0 * done / total));
                detail.setText(String.format(Locale.ROOT, "%.1f / %.1f MiB", done / 1048576.0, total / 1048576.0));
            } else {
                detail.setText("Checking the game files...");
            }
        });
    }

    public void onGameDataReady(String error) {
        ui.post(() -> {
            ready = true;
            if (dialog != null) { dialog.dismiss(); dialog = null; }
            if (error != null && !isFinishing() && !isDestroyed()) {
                new AlertDialog.Builder(this).setTitle("Cannot prepare game data")
                    .setMessage(error).setPositiveButton("Close", (d, which) -> finish())
                    .setCancelable(false).show();
            } else if (error == null) { prepared = true; launchIfReady(); }
        });
    }
}
