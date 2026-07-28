package com.zheliu.usbbook;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.provider.Settings;
import android.view.Gravity;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import java.io.BufferedReader;
import java.io.InputStreamReader;

public final class MainActivity extends Activity {
    private TextView status;

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        setContentView(buildPage());
    }

    @Override public void onResume() {
        super.onResume();
        updateStatus();
    }

    private View buildPage() {
        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setPadding(dp(28), dp(22), dp(28), dp(18));
        page.setBackgroundColor(Color.WHITE);

        page.addView(text("USB 传书", 28, Typeface.BOLD));
        TextView intro = text("用数据线把文件直接送进书库。", 16, Typeface.NORMAL);
        intro.setPadding(0, dp(8), 0, dp(14));
        page.addView(intro);

        status = text("", 18, Typeface.BOLD);
        status.setGravity(Gravity.CENTER_VERTICAL);
        status.setPadding(dp(20), 0, dp(20), 0);
        status.setBackground(box());
        page.addView(status, params(-1, dp(72)));

        TextView steps = text(
                "1. 用 USB 线连接 Mac\n" +
                "2. Mac 双击“Poke3传书”\n" +
                "3. 选择 EPUB、PDF 或 CBZ\n" +
                "4. 文件进入 Books/Inbox",
                17, Typeface.NORMAL);
        steps.setLineSpacing(dp(6), 1f);
        steps.setPadding(dp(6), dp(18), dp(6), dp(14));
        page.addView(steps, params(-1, 0, 1f));

        page.addView(button("打开系统设置", v -> openSystemSettings()),
                params(-1, dp(70)));
        page.addView(space(dp(10)));
        page.addView(button("打开文石存储", v -> openStorage()),
                params(-1, dp(70)));

        TextView note = text("传完可直接拔线；Wi-Fi 无需开启。", 15, Typeface.NORMAL);
        note.setGravity(Gravity.CENTER);
        note.setPadding(0, dp(18), 0, 0);
        page.addView(note);
        return page;
    }

    private void updateStatus() {
        int enabled = Settings.Global.getInt(
                getContentResolver(), Settings.Global.ADB_ENABLED, 0);
        String usbConfig = readProperty("sys.usb.config");
        if (enabled == 1 && usbConfig.contains("adb")) {
            status.setText("✓ ADB USB 已连接就绪");
        } else if (enabled == 1) {
            status.setText("⚠ 调试开关已开，ADB 接口未出现");
        } else {
            status.setText("⚠ USB 调试尚未开启");
        }
    }

    private String readProperty(String key) {
        try {
            Process process = Runtime.getRuntime().exec(
                    new String[]{"/system/bin/getprop", key});
            BufferedReader reader = new BufferedReader(
                    new InputStreamReader(process.getInputStream()));
            String value = reader.readLine();
            process.waitFor();
            return value == null ? "" : value.trim();
        } catch (Exception error) {
            return "";
        }
    }

    private void openSystemSettings() {
        try {
            Intent settings = new Intent("com.onyx.action.SETTING");
            settings.setPackage("com.onyx");
            startActivity(settings);
        } catch (Exception error) {
            Toast.makeText(this, "无法打开设置", Toast.LENGTH_SHORT).show();
        }
    }

    private void openStorage() {
        try {
            Intent storage = new Intent("com.onyx.action.STORAGE");
            storage.setPackage("com.onyx");
            startActivity(storage);
        } catch (Exception error) {
            Toast.makeText(this, "无法打开文石存储", Toast.LENGTH_SHORT).show();
        }
    }

    private TextView button(String label, View.OnClickListener action) {
        TextView view = text(label, 22, Typeface.BOLD);
        view.setGravity(Gravity.CENTER);
        view.setBackground(box());
        view.setOnClickListener(action);
        return view;
    }

    private GradientDrawable box() {
        GradientDrawable box = new GradientDrawable();
        box.setColor(Color.WHITE);
        box.setStroke(dp(2), Color.BLACK);
        box.setCornerRadius(dp(8));
        return box;
    }

    private TextView text(String value, int size, int style) {
        TextView view = new TextView(this);
        view.setText(value);
        view.setTextSize(size);
        view.setTextColor(Color.BLACK);
        view.setTypeface(Typeface.DEFAULT, style);
        return view;
    }

    private View space(int height) {
        View view = new View(this);
        view.setLayoutParams(params(1, height));
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
