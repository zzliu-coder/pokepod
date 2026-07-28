package com.zheliu.pokehome;

import android.app.Activity;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.BatteryManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.view.Gravity;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

public final class MainActivity extends Activity {
    private TextView battery;

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        setContentView(buildHome());
        applyPowerDefaults();
        new Handler(Looper.getMainLooper()).postDelayed(this::applyPowerDefaults, 40000);
    }

    @Override public void onResume() {
        super.onResume();
        updateBattery();
        applyPowerDefaults();
        new Handler(Looper.getMainLooper()).postDelayed(this::applyPowerDefaults, 40000);
    }

    private View buildHome() {
        int pad = dp(22);
        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setPadding(pad, dp(20), pad, dp(16));
        page.setBackgroundColor(Color.WHITE);

        LinearLayout header = new LinearLayout(this);
        header.setOrientation(LinearLayout.HORIZONTAL);
        header.setGravity(Gravity.CENTER_VERTICAL);

        LinearLayout identity = new LinearLayout(this);
        identity.setOrientation(LinearLayout.VERTICAL);
        TextView title = text("Poke", 31, Typeface.BOLD);
        identity.addView(title);
        TextView subtitle = text("阅读与记录", 16, Typeface.NORMAL);
        subtitle.setTextColor(Color.DKGRAY);
        identity.addView(subtitle);
        header.addView(identity, params(0, -2, 1f));

        battery = text("电量", 15, Typeface.NORMAL);
        battery.setGravity(Gravity.END);
        header.addView(battery, params(dp(90), -1));
        page.addView(header, params(-1, dp(78)));

        LinearLayout mainRow = row();
        mainRow.addView(primaryEntry(
                "阅读", "书籍与漫画", v -> openReader()), tileParams(1f));
        mainRow.addView(primaryEntry(
                "语音胶囊", "随手录下想法", v -> openPackage(
                        "com.zheliu.pokecapsule", "语音胶囊尚未安装")), tileParams(1f));
        page.addView(mainRow, params(-1, 0, 1.25f));

        LinearLayout firstTools = row();
        firstTools.addView(toolEntry(
                "USB 传书", v -> openPackage(
                        "com.zheliu.usbbook", "USB 传书尚未安装")), tileParams(1f));
        firstTools.addView(toolEntry(
                "存储", v -> openOnyxStorage()), tileParams(1f));
        page.addView(firstTools, params(-1, 0, 0.75f));

        LinearLayout secondTools = row();
        secondTools.addView(toolEntry(
                "应用", v -> openOnyxApps()), tileParams(1f));
        secondTools.addView(toolEntry(
                "设置", v -> openOnyxSettings()), tileParams(1f));
        page.addView(secondTools, params(-1, 0, 0.75f));

        TextView note = text("常用功能都在这一页", 13, Typeface.NORMAL);
        note.setTextColor(Color.DKGRAY);
        note.setGravity(Gravity.CENTER);
        page.addView(note, params(-1, dp(34)));
        return page;
    }

    private LinearLayout row() {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        return row;
    }

    private LinearLayout primaryEntry(
            String name,
            String detail,
            View.OnClickListener action) {
        LinearLayout view = new LinearLayout(this);
        view.setOrientation(LinearLayout.VERTICAL);
        view.setGravity(Gravity.CENTER);
        view.setPadding(dp(14), dp(12), dp(14), dp(12));
        TextView nameView = text(name, 25, Typeface.BOLD);
        nameView.setGravity(Gravity.CENTER);
        view.addView(nameView);
        TextView detailView = text(detail, 15, Typeface.NORMAL);
        detailView.setTextColor(Color.DKGRAY);
        detailView.setGravity(Gravity.CENTER);
        view.addView(detailView);
        decorateEntry(view, action);
        return view;
    }

    private TextView toolEntry(String name, View.OnClickListener action) {
        TextView view = text(name, 21, Typeface.BOLD);
        view.setGravity(Gravity.CENTER);
        view.setPadding(dp(10), dp(10), dp(10), dp(10));
        decorateEntry(view, action);
        return view;
    }

    private void decorateEntry(View view, View.OnClickListener action) {
        GradientDrawable box = new GradientDrawable();
        box.setColor(Color.WHITE);
        box.setStroke(dp(2), Color.BLACK);
        box.setCornerRadius(dp(8));
        view.setBackground(box);
        view.setOnClickListener(action);
        view.setFocusable(true);
    }

    private void openReader() {
        openPackage("org.koreader.launcher", "KOReader 尚未安装");
    }

    private void openPackage(String packageName, String missingMessage) {
        Intent launch = getPackageManager().getLaunchIntentForPackage(packageName);
        if (launch == null) {
            Toast.makeText(this, missingMessage, Toast.LENGTH_SHORT).show();
            return;
        }
        startActivity(launch);
    }

    private void openOnyxStorage() {
        openOnyxAction("com.onyx.action.STORAGE");
    }

    private void openOnyxApps() {
        Intent apps = new Intent("com.onyx.intent.action.MAIN_ACTIVITY");
        apps.setPackage("com.onyx");
        apps.putExtra("json", "{\"action\":\"OPEN_APPS\",\"intentFlag\":-1}");
        startActivity(apps);
    }

    private void openOnyxSettings() {
        openOnyxAction("com.onyx.action.SETTING");
    }

    private void openOnyxAction(String action) {
        Intent intent = new Intent(action);
        intent.setPackage("com.onyx");
        startActivity(intent);
    }

    private void updateBattery() {
        Intent batteryState = registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
        int level = batteryState == null ? -1 : batteryState.getIntExtra(BatteryManager.EXTRA_LEVEL, -1);
        battery.setText(level < 0 ? "电量 --" : "电量 " + level + "%");
    }

    private void applyPowerDefaults() {
        if (Settings.System.canWrite(this)) {
            Settings.System.putInt(getContentResolver(), Settings.System.SCREEN_OFF_TIMEOUT, 360000);
        }
    }

    private TextView text(String value, int size, int style) {
        TextView view = new TextView(this);
        view.setText(value);
        view.setTextSize(size);
        view.setTextColor(Color.BLACK);
        view.setTypeface(Typeface.DEFAULT, style);
        return view;
    }

    private LinearLayout.LayoutParams params(int width, int height) {
        return new LinearLayout.LayoutParams(width, height);
    }

    private LinearLayout.LayoutParams params(int width, int height, float weight) {
        return new LinearLayout.LayoutParams(width, height, weight);
    }

    private LinearLayout.LayoutParams tileParams(float weight) {
        LinearLayout.LayoutParams value = params(0, -1, weight);
        value.setMargins(dp(5), dp(5), dp(5), dp(5));
        return value;
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }
}
