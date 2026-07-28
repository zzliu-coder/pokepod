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
        int pad = dp(28);
        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setPadding(pad, dp(24), pad, dp(20));
        page.setBackgroundColor(Color.WHITE);

        TextView title = text("Poke", 31, Typeface.BOLD);
        page.addView(title);
        TextView subtitle = text("我的阅读设备", 16, Typeface.NORMAL);
        subtitle.setTextColor(Color.DKGRAY);
        page.addView(subtitle, params(-1, dp(38)));

        battery = text("电量", 15, Typeface.NORMAL);
        battery.setGravity(Gravity.RIGHT);
        page.addView(battery, params(-1, dp(35)));

        page.addView(entry("阅读", "KOReader · EPUB / PDF / CBZ", v -> openReader()), params(-1, 0, 1f));
        page.addView(entry("存储", "文档、下载与安装包", v -> openOnyxStorage()), params(-1, 0, 1f));
        page.addView(entry("应用", "文石应用管理", v -> openOnyxApps()), params(-1, 0, 1f));
        page.addView(entry("设置", "网络、电源与设备管理", v -> openOnyxSettings()), params(-1, 0, 1f));

        TextView note = text("离线优先 · 无账号 · 无商店 · 无后台同步", 13, Typeface.NORMAL);
        note.setTextColor(Color.DKGRAY);
        note.setGravity(Gravity.CENTER);
        page.addView(note, params(-1, dp(34)));
        return page;
    }

    private TextView entry(String name, String detail, View.OnClickListener action) {
        TextView view = text(name + "\n" + detail, 23, Typeface.BOLD);
        view.setLineSpacing(dp(3), 1f);
        view.setGravity(Gravity.CENTER_VERTICAL);
        view.setPadding(dp(24), 0, dp(24), 0);
        GradientDrawable box = new GradientDrawable();
        box.setColor(Color.WHITE);
        box.setStroke(dp(2), Color.BLACK);
        box.setCornerRadius(dp(8));
        view.setBackground(box);
        view.setOnClickListener(action);
        return view;
    }

    private void openReader() {
        Intent launch = getPackageManager().getLaunchIntentForPackage("org.koreader.launcher");
        if (launch == null) {
            Toast.makeText(this, "KOReader 尚未安装", Toast.LENGTH_SHORT).show();
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

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }
}
